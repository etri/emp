#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/kvm_host.h>
#include <linux/sort.h>
#include "config.h"
#include "emp_type.h"
#include "gpa.h"
#include "vm.h"
#include "alloc.h"
#include "hva.h"
#include "glue.h"
#include "reclaim.h"
#include "block-flag.h"
#include "page_mgmt.h"
#include "kvm_mmu.h"
#include "block.h"
#include "debug.h"
#ifdef CONFIG_EMP_USER
#include "cow.h"
#endif
#include "pcalloc.h"

#ifdef CONFIG_EMP_BLOCK
static inline void emp_init_gpa(struct emp_mm *b, struct emp_gpa *g) {
	memset(g, 0, sizeof(struct emp_gpa));
#ifdef CONFIG_EMP_DEBUG_GPA_STATE
	init_gpa_flags(g, 0);
#endif
	____emp_gpa_init_lock(g);
	g->r_state = GPA_INIT;
	set_gpa_remote_page_free(g);
	g->last_mr_id = -1;
	init_gpa_subblock_order(g, bvma_subblock_order(b));
	set_gpa_block_order(g, BLOCK_MAX_ORDER);
	set_gpa_max_block_order(g, BLOCK_MAX_ORDER);
	init_progress_info(g);
	init_gpa_contrib_inactive(g);
	debug_gpa_refcnt_init(g);
}
#else
static inline void emp_init_gpa(struct emp_mm *b, struct emp_gpa *g) {
	memset((struct emp_gpa *)g, 0, sizeof(struct emp_gpa));
#ifdef CONFIG_EMP_DEBUG_GPA_STATE
	init_gpa_flags(g, 0);
#endif
	____emp_gpa_init_lock(g);
	g->r_state = GPA_INIT;
	set_gpa_remote_page_free(g);
	g->last_mr_id = -1;
	init_progress_info(g);
	init_gpa_contrib_inactive(g);
	debug_gpa_refcnt_init(g);
}
#endif

static struct page *
cleanup_gpa(struct emp_mm *, struct emp_gpa *);

#ifdef CONFIG_EMP_VM
static void ack_flush(void *_completed)
{
}

/**
 * make_all_cpus_req - Make all the CPUs to call a request
 * @param kvm kvm info
 * @param req request to call
 *
 * call IPI for active vCPUs.
 * NOTE: from linux 4.18-0-425 (5.18.0), @req must be a built-in constant.
 *       so we change this function to a macro function.
 */
#define make_all_cpus_req(kvm, req) \
do { \
	unsigned long i; \
	cpumask_var_t cpus; \
	struct kvm_vcpu *vcpu; \
\
	zalloc_cpumask_var(&cpus, GFP_ATOMIC); \
\
	if (unlikely(cpus == NULL)) { \
		kvm_for_each_vcpu(i, vcpu, (kvm)) { \
			kvm_make_request((req), vcpu); \
		} \
\
		smp_call_function_many(cpu_online_mask, ack_flush, NULL, 1); \
	} else { \
		int cpu, me; \
		me = get_cpu(); \
		kvm_for_each_vcpu(i, vcpu, (kvm)) { \
			kvm_make_request((req), vcpu); \
\
			cpu = vcpu->cpu; \
			if (cpu < 0 || cpu == me) \
				continue; \
\
			if (kvm_vcpu_exiting_guest_mode(vcpu) != OUTSIDE_GUEST_MODE) \
				cpumask_set_cpu(cpu, cpus); \
		} \
\
		if (!cpumask_empty(cpus)) \
			smp_call_function_many(cpus, ack_flush, NULL, 1); \
\
		put_cpu(); \
		free_cpumask_var(cpus); \
	} \
} while (0)

/**
 * make_all_cpus_req_no_IPI - Make all the CPUs to flush TLB, but do not call IPI
 * @param kvm kvm info
 * @param req request to call
 *
 * NOTE: from linux 4.18-0-425 (5.18.0), @req must be a built-in constant.
 *       so we change this function to a macro function.
 */
#define make_all_cpus_req_no_IPI(kvm, req) \
do { \
	unsigned long i, me; \
	struct kvm_vcpu *vcpu; \
	me = get_cpu(); \
	kvm_for_each_vcpu(i, vcpu, (kvm)) \
		kvm_make_request((req), vcpu); \
	put_cpu(); \
} while (0)

/**
 * __flush_remote_tlbs - Make all the CPUs to flush TLB
 * @param bvma bvma data structure
 * @param need to flush using IPI?
 *
 * Invalidating tlbs in virtualized environment is inefficient.
 * TLB is invalidated when tlb_dirty is set.
 */
static bool flush_remote_tlbs(struct emp_mm *bvma, bool need_tlb_flush_ipi)
{
	struct kvm *kvm = bvma->ekvm.kvm;
	long dirty_count __maybe_unused = load_acquire_kvm_tlbs_dirty_or_0(kvm);

	if (need_tlb_flush_ipi) {
		make_all_cpus_req(kvm, KVM_REQ_TLB_FLUSH);
#ifdef CONFIG_EMP_STAT
		emp_stat_inc(bvma, remote_tlb_flush);
#endif

		/* checks whether there was tlb invalidation by other threads
		 * while executing this function. */
		cmpxchg_kvm_tlbs_dirty(kvm, dirty_count, 0);

		return true;
	}

	make_all_cpus_req_no_IPI(kvm, KVM_REQ_TLB_FLUSH);
#ifdef CONFIG_EMP_STAT
	emp_stat_inc(bvma, remote_tlb_flush_no_ipi);
#endif
	return false;
}

static unsigned long get_num_low_memory_pages(struct emp_mm *e)
{
	int low_memory_pages = LOW_MEMORY_REGION_SIZE >> PAGE_SHIFT;
	debug_assert(e->ekvm.kvm != NULL);
	if (bvma_block_size(e) > low_memory_pages)
		return bvma_block_size(e);
	else
		return low_memory_pages;
}
#endif /* CONFIG_EMP_VM */

#ifdef CONFIG_EMP_BLOCK
static inline void update_gpa_order(struct emp_gpa *g, int order) {
	set_gpa_block_order(g, order);
	set_gpa_max_block_order(g, order);
}
#else
#define update_gpa_order(g, o) debug_assert((o) == 0)
#endif

#ifdef CONFIG_EMP_EXT
static inline void 
emp_ext_init_gpa(struct emp_mm *e, struct emp_gpa *g, unsigned long i) {
	if (emp_ext.init_gpa)
		emp_ext.init_gpa(e, g, i);
}
#else
#define emp_ext_init_gpa(e, g, i) do {} while(0)
#endif

static inline void
__set_gpadesc(struct emp_mm *emm, unsigned long idx, struct emp_gpa *g,
					struct gpadesc_region *region)
{
	emp_init_gpa(emm, g);
	update_gpa_order(g, region->alloc_order);
#ifdef CONFIG_EMP_VM
	if (region->lowmem_block)
		set_gpa_flags_if_unset(g, GPA_LOWMEM_BLOCK_MASK);
#endif
#ifdef CONFIG_EMP_USER
	if (region->partial_map)
		set_gpa_flags_if_unset(g, GPA_PARTIAL_MAP_MASK);
#endif
	emp_ext_init_gpa(emm, g, idx);
}

struct emp_gpa * COMPILER_DEBUG
new_gpadesc(struct emp_vmr *vmr, unsigned long idx) {
	struct gpadesc_region *region;
	unsigned long i, head_idx, end_idx, len;
	int desc_order;
	struct emp_mm *emm = vmr->emm;
	struct emp_gpa **gpa_dir = vmr->descs->gpa_dir;
	struct emp_gpa *new, *cur;

	/* for_all_gpa_heads() may fall into this condition */
	if (unlikely(idx >= vmr->descs->gpa_len))
		return NULL;

	region = get_gpadesc_region(vmr->descs, idx);
	desc_order = region->alloc_order - bvma_subblock_order(emm);
	head_idx = _emp_get_block_head_index(vmr, idx, desc_order);
	len = 1UL << desc_order;
	end_idx = head_idx + len;

	new = alloc_gpadesc(emm, desc_order);
	if (unlikely(!new)) {
		printk(KERN_ERR "%s: failed to allocate new gpa descriptor. "
				"emm: %d order: %d\n",
				__func__, emm->id, desc_order);
		return NULL;
	}

	for (i = head_idx, cur = new; i < end_idx; i++, cur++)
		__set_gpadesc(emm, i, cur, region);

	__emp_lock_block(new);
	cur = set_gpa_dir_new(vmr, gpa_dir, head_idx, new);
	if (unlikely(cur != NULL)) {
		/* race condition */
		/* free the allocated resource */
		__emp_unlock_block(new);
		free_gpadesc(emm, desc_order, new);
		/* wait for the other thread completed */
		__emp_lock_block(cur);
		emp_unlock_block(cur);
		debug_assert(gpa_dir[idx] != NULL);
		return gpa_dir[idx];
	}

	debug_assert(gpa_dir[head_idx] == new);

	for (i = head_idx + 1, cur = new + 1; i < end_idx; i++, cur++) {
		debug_assert(gpa_dir[i] == NULL);
		set_gpa_dir_new(vmr, gpa_dir, i, cur);
	}
	emp_unlock_block(new);

	return gpa_dir[idx];
}

static inline void __sort_boundaries(unsigned long *boundary, int num_boundary)
{
	int i, j, min_idx;
	unsigned long min_val;

	for (i = 0; i < num_boundary; i++) {
		min_val = boundary[i];
		min_idx = i;
		for (j = i + 1; j < num_boundary; j++) {
			if (min_val > boundary[j]) {
				min_val = boundary[j];
				min_idx = j;
			}
		}

		if (min_idx == i)
			continue;

		// swap two values
		boundary[min_idx] = boundary[i];
		boundary[i] = min_val;
	}
}

/* sort gpadesc regions by its range size */
static inline void __sort_regions(struct gpadesc_region *regions, int num_region)
{
	int i, j, max_idx;
	unsigned long val, max_val;
	struct gpadesc_region tmp;

	for (i = 0; i < num_region; i++) {
		max_val = regions[i].end - regions[i].start;
		max_idx = i;
		for (j = i + 1; j < num_region; j++) {
			val = regions[j].end - regions[j].start;
			if (max_val < val) {
				max_val = val;
				max_idx = j;
			}
		}

		if (max_idx == i)
			continue;

		memcpy(&tmp, &regions[i], sizeof(struct gpadesc_region));
		memcpy(&regions[i], &regions[max_idx], sizeof(struct gpadesc_region));
		memcpy(&regions[max_idx], &tmp, sizeof(struct gpadesc_region));
	}
}

/* set gpadesc regions (vmr->descs->regions)
 * There are several boundaries:
 *   0: the beginning
 *   partial_map_at_head: if vma->vm_start is not subblock-aligned,
 *                        the first gpa has "PARTIAL_MAP_MASK"
 *   partial_map_at_tail: if vma->vm_end is not subblock-aligned,
 *                        the last gpa has "PARTIAL_MAP_MASK"
 *   block_aligned_start: if vma->vm_start is not block-aligned,
 *                        the first few subblocks do not belong to any other blocks.
 *                        That is, block_order = subblock_order.
 *   block_aligned_end: if vma->vm_end is not block-aligned,
 *                      the last few subblocks do not belong to any other blocks.
 *                      That is, block_order = subblock_order.
 *   low_memory_end: for VMs, the first 2MB region has a limitation on block_order.
 *                   Thus, block_order is less than or equal to LOW_MEMORY_MAX_ORDER.
 *   gpa_len: the end
 *
 * This function creates the gpa descriptor regions based on the boundaries,
 * and sorts the regions by its range size.
 */
static void
set_gpadesc_regions(struct emp_vmr *vmr,
			unsigned long vm_start, unsigned long vm_end,
			unsigned long sb_at_head, unsigned long sb_at_tail)
{
#if defined(CONFIG_EMP_VM) || defined(CONFIG_EMP_BLOCK)
	struct emp_mm *emm = vmr->emm;
#endif
	struct emp_vmdesc *desc = vmr->descs;
	struct gpadesc_region *regions = desc->regions;
	unsigned long gpa_len = desc->gpa_len;
	unsigned long boundary[GPADESC_MAX_REGION];
	int num_boundary, i, num_region;
	u8 b_order, sb_order;
	unsigned long block_aligned_start, block_aligned_end;
#ifdef CONFIG_EMP_USER
	bool partial_at_head, partial_at_tail;
#endif
#ifdef CONFIG_EMP_VM
	u8 low_order;
	unsigned long low_memory_end;
#endif

	b_order = bvma_block_order(emm);
	sb_order = bvma_subblock_order(emm);

#ifdef CONFIG_EMP_VM
	low_order = (u8) LOW_MEMORY_MAX_ORDER;

	/* Set low memory end */
	if (emm->ekvm.kvm && vmr == emm->ekvm.lowmem_vmr) {
		/* TODO: how can we know GFN of vm_start?
		 * low memory region is the first 2MB of VM, and we need to
		 * restrict the maximum order of the region. Unfortunately, we
		 * only know HVA range here. Thus, we use a heuristic: the first
		 * vmr this emp_mm maps is the first memory region (numa node)
		 * and its GFN is started from 0.
		 * We need to revise this. For example, add an IOCTL and let
		 * EMP know the GFN of each memory region (numa node) before VM
		 * starts.
		 */
		low_memory_end = get_num_low_memory_pages(emm) >> sb_order;
	} else
		low_memory_end = 0;
#endif

	/* gather information */
#ifdef CONFIG_EMP_USER
	partial_at_head = vmr->vm_start != vm_start ? true : false;
	partial_at_tail = vmr->vm_end != vm_end ? true : false;
#endif
	block_aligned_start = sb_at_head;
	block_aligned_end = sb_at_tail < gpa_len ? gpa_len - sb_at_tail : 0;

	/* add boundaries */
	num_boundary = 0;
#ifdef CONFIG_EMP_VM
	if (low_memory_end > 0)
		boundary[num_boundary++] = low_memory_end;
#endif

#ifdef CONFIG_EMP_USER
	if (partial_at_head && gpa_len > 0)
		boundary[num_boundary++] = 1;

	// partial_at_tail makes new boundary only if gpa_len > 1
	if (partial_at_tail && gpa_len > 1)
		boundary[num_boundary++] = gpa_len - 1;
#endif

	// if gpa_len < num_subblock_in_block,
	// sb_at_head and sb_at_tail is larger than gpa_len,
	// and no new boundaries are made.
	// See the comment at allocate_gpas()
	if (sb_at_head > 0 && sb_at_head < gpa_len)
		boundary[num_boundary++] = block_aligned_start;

	if (sb_at_tail > 0 && sb_at_tail < gpa_len)
		boundary[num_boundary++] = block_aligned_end;

	boundary[num_boundary++] = gpa_len;

	/* sort boundaries */
	__sort_boundaries(boundary, num_boundary);

	num_region = 0;
	for (i = 0; i < num_boundary; i++) {
		struct gpadesc_region *curr;
		struct gpadesc_region *prev;
		u8 order;
		curr = &regions[num_region];
		prev = num_region > 0 ? &regions[num_region - 1] : NULL;

		// skip duplicated boundaries
		if (prev && prev->end == boundary[i])
			continue;

		curr->start = prev ? prev->end : 0;
		curr->end = boundary[i];
		// if gpa_len < num_subblock_in_block,
		// block_aligned_start > gpa_len and block_aligned_end < gpa_len.
		// Thus, always fall into the first condition.
		order = b_order;
		if ((curr->end <= block_aligned_start
				|| curr->start >= block_aligned_end)
					&& order > sb_order)
			order = sb_order;

#ifdef CONFIG_EMP_VM
		if (curr->end <= low_memory_end && order > low_order)
			order = low_order;
#endif

		curr->alloc_order = order;

#ifdef CONFIG_EMP_VM
		curr->lowmem_block = curr->end <= low_memory_end
						? true : false;
#endif
#ifdef CONFIG_EMP_USER
		if (unlikely(partial_at_head && curr->end <= 1))
			curr->partial_map = true;
		else if (unlikely(partial_at_tail && curr->start >= gpa_len - 1))
			curr->partial_map = true;
		else
			curr->partial_map = false;
#endif

		num_region++;
	}

	/* sort regions by its range (end - start) */
	__sort_regions(regions, num_region);
	desc->num_region = num_region;

#ifdef CONFIG_EMP_DEBUG
	for (i = 0; i < num_region; i++) {
		struct gpadesc_region *r = &regions[i];
		printk(KERN_INFO "%s: emm(%d) vmr(%d) region(%d) "
					"start: 0x%lx end: 0x%lx "
					"block_order: %d lowmem: %d partial: %d\n",
					__func__,
					emm->id, emp_vmr_dbgid(vmr), i,
					r->start, r->end,
					r->alloc_order,
#ifdef CONFIG_EMP_VM
					r->lowmem_block ? 1 : 0,
#else
					-1,
#endif
#ifdef CONFIG_EMP_USER
					r->partial_map ? 1 : 0
#else
					-1
#endif
					);
	}
#endif
}

static void __prepare_gpadesc_alloc(struct emp_mm *emm, int order)
{
	struct kmem_cache *cachep;
	char name[32];
	gpadesc_alloc_lock(emm);
	cachep = __get_gpadesc_alloc(emm, order);
	gpadesc_alloc_unlock(emm);
	if (cachep)
		return;
	scnprintf(name, sizeof(name), "gpadesc_alloc%d-%d", emm->id, order);
	/* With the align in 3rd parameter, the offset in the block can
	 * be calculated by the memory address.
	 */
	cachep = emp_kmem_cache_create(name,
					sizeof(struct emp_gpa) << order,
					sizeof(struct emp_gpa) << order,
					0, NULL);
	gpadesc_alloc_lock(emm);
	if (__get_gpadesc_alloc(emm, order))
		/* somebody filled it */
		emp_kmem_cache_destroy(cachep);
	else
		set_gpadesc_alloc(emm, order, cachep);
	gpadesc_alloc_unlock(emm);
}

static void prepare_gpadesc_alloc(struct emp_mm *emm, struct emp_vmdesc *desc)
{
	int r;
	int order;

	for (r = 0; r < desc->num_region; r++) {
		order = desc->regions[r].alloc_order - bvma_subblock_order(emm);
		__prepare_gpadesc_alloc(emm, order);
	}

	/* __split_vmdesc() may require desc_order==0 */
	__prepare_gpadesc_alloc(emm, 0);
}

#define PAGE_ROUND_UP(s) ((s + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
/**
 * allocate_gpas - Allocate gpas for the VM's address space
 * @param emm emm data structure
 * @param vma virtual memory area structure
 * @param page_order size of the page
 *
 * @retval n: Error
 * @retval 0: Success
 *
 * Allocate gpas to manage all the pages working on EMP \n
 * GPA: a page descriptor for each page in EMP system
 */
static int COMPILER_DEBUG
allocate_gpas(struct emp_mm *emm, struct emp_vmr *vmr)
{
	unsigned long gpa_len;
	unsigned long vm_start, vm_end;
	unsigned long sb_at_head; // number of subblocks which are not block-aligned at head
	unsigned long sb_at_tail; // number of subblocks which are not block-aligned at tail
	unsigned long gpa_dir_alloc_size, gpa_dir_offset;
	unsigned long va_sb_order;

	va_sb_order = bvma_va_subblock_order(emm);

	vm_start = VA_ROUND_DOWN_ORDER(vmr->vm_start, va_sb_order);
	vm_end = VA_ROUND_UP_ORDER(vmr->vm_end, va_sb_order);

	/* number of subblocks in the first partial block.
	 * If vm_start is aligned in block size, this is 0.
	 * If vm_end - vm_start <= block size, this counts the number of subblocks
	 *					in [vm_start, block aligned vm_end]
	 */
	sb_at_head = (vm_start >> va_sb_order) & bvma_sib_mask(emm);
	sb_at_head = sb_at_head > 0 ? bvma_sib_size(emm) - sb_at_head : 0;

	/* number of subblocks in the last partial block.
	 * If vm_end is aligned in block size, this is 0.
	 * If vm_end - vm_start <= block size, this counts the number of subblocks
	 * 					in [block aligned vm_start, vm_end]
	 */
	sb_at_tail = (vm_end >> va_sb_order) & bvma_sib_mask(emm);

	/* For partial subblock, its block order should be same with subblock
	 * order. If there is no partial block but partial subblock, we split
	 * the block that the partial subblocks belong to.
	 * The partial subblocks exist when vmr->vm_start/end != vm_start/end.
	 */
	if (sb_at_head == 0 && vmr->vm_start != vm_start)
		sb_at_head = bvma_sib_size(emm);
	if (sb_at_tail == 0 && vmr->vm_end != vm_end)
		sb_at_tail = bvma_sib_size(emm);

	gpa_len = (vm_end - vm_start) >> va_sb_order;

	dprintk("%s vmr:0x%lx--0x%lx gpa_len: 0x%lx block_order: 0x%x sb_order: 0x%x va_sb_order: 0x%lx sb_at_head: 0x%lx sb_at_tail: 0x%lx\n",
			__func__,
			vmr->vm_start, vmr->vm_end,
			gpa_len,
			bvma_block_order(emm),
			bvma_subblock_order(emm),
			va_sb_order,
			sb_at_head, sb_at_tail);

	might_sleep();

	/* allocate gpas */
	gpa_dir_alloc_size = gpa_len * sizeof(struct emp_gpa *);

	// Pads at the head make that entries of a block are page-aligned.
	if (sb_at_head) {
		gpa_dir_offset = (bvma_sib_size(emm) - sb_at_head)
						* sizeof(struct emp_gpa *);
		if (unlikely(gpa_dir_offset > PAGE_SIZE))
			gpa_dir_offset -= gpa_dir_offset & ~(PAGE_SIZE - 1);
		gpa_dir_alloc_size += gpa_dir_offset;
	} else
		gpa_dir_offset = 0;

	// Pads at the tail make get_gpadesc() from for_all_gpa*() works
	// at the end of the loop.
	if (sb_at_tail)
		// num_subblock_in_block() of the last gpa will be 1
		gpa_dir_alloc_size += sizeof(struct emp_gpa *);
	else
		gpa_dir_alloc_size += bvma_sib_size(emm) * sizeof(struct emp_gpa *);

	// Make sure that allocate page-aliged size for gpa_dir_alloc
	gpa_dir_alloc_size = PAGE_ROUND_UP(gpa_dir_alloc_size);
#ifdef CONFIG_EMP_PREFER_DCPMM
	vmr->descs->gpa_dir_alloc = __vmalloc_node(gpa_dir_alloc_size, 1,
						GFP_KERNEL | __GFP_ZERO, 2,
						__builtin_return_address(0));
#else
	vmr->descs->gpa_dir_alloc = emp_vzalloc(gpa_dir_alloc_size);
#endif
	if (!vmr->descs->gpa_dir_alloc) {
		printk(KERN_ERR "%s: failed to allocate memory for gpa directory.\n", __func__);
		return -ENOMEM;
	}

	vmr->descs->gpa_dir = (struct emp_gpa **)(vmr->descs->gpa_dir_alloc + gpa_dir_offset);
	vmr->descs->gpa_dir_alloc_size = gpa_dir_alloc_size;

	/* update vmr fields */
	vmr->descs->gpa_len = gpa_len;
	vmr->descs->vm_base = vm_start;
	vmr->descs->block_aligned_start = sb_at_head;

	set_gpadesc_regions(vmr, vm_start, vm_end, sb_at_head, sb_at_tail);
	prepare_gpadesc_alloc(emm, vmr->descs);

	dprintk("%s: total gpa descriptor len: %ld directory_size: 0x%lx MB (0x%lx)\n",
			__func__, vmr->descs->gpa_len,
			gpa_dir_alloc_size >> MB_ORDER, gpa_dir_alloc_size);
	return 0;
}

/**
 * is_head_page - Check if the page is a head of the block
 * @param page page structure
 * @param page_order size of the page
 *
 * @retval true: head
 * @retval false: not head
 *
 * Returns true if the given page is the head of a block
 */
static inline bool is_head_page(struct page *page, int page_order) {
	return ((page_to_pfn(page) & ((1 << page_order) - 1)) == 0);
}

/**
 * free_gpa - Free the page in local memory
 * @param bvma bvma data structure
 * @param gpa gpa
 * @param vcpu working vcpu ID
 *
 * Free the page and push it to free list
 */
// free_gpa must be called in reverse order
static void free_gpa(struct emp_mm *bvma, struct emp_gpa *gpa, struct vcpu_var *cpu)
{
	struct page *gpa_page;

#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	if (gpa->local_page)
		debug_page_ref_mark(-100, gpa->local_page, 0);
#endif
	gpa_page = cleanup_gpa(bvma, gpa);
	if (gpa_page) {
		clear_page_state(gpa_page);
		_emp_lock_page(gpa_page);
	}

	if (!gpa_page)
		return;

	debug_check_head(gpa_page);
	push_free_page_list(bvma, gpa_page, cpu);
}

#ifdef CONFIG_EMP_VM
/**
 * hva_to_gpa - Translate host virtual address to guest physical address
 * @param bvma bvma data structure
 * @param hva host virtual address
 * @param ms memory slot
 *
 * Translate hva to gpa using memory slots
 */
u64 hva_to_gpa(struct emp_mm *bvma, u64 hva, struct kvm_memory_slot **ms)
{
	int i, idx;
	u64 gpa = -1;
	struct kvm_memslots *slots;
	size_t bkt __maybe_unused;
	struct kvm_memory_slot *memslot;

	if (unlikely(bvma->ekvm.kvm == NULL))
		return gpa;

	idx = srcu_read_lock(&bvma->ekvm.kvm->srcu);
	for (i = 0; i < kvm_nr_memslot(bvma->ekvm.kvm); i++) {
		slots = __kvm_memslots(bvma->ekvm.kvm, i);
		emp_kvm_for_each_memslot(memslot, bkt, slots) {
			if (hva >= (memslot->userspace_addr + 
					(memslot->npages << PAGE_SHIFT)) ||
				hva < memslot->userspace_addr)
				continue;

			*ms = memslot;
			gpa = hva_to_gfn_memslot(hva, memslot) << PAGE_SHIFT;
		}
	}
	srcu_read_unlock(&bvma->ekvm.kvm->srcu, idx);

	return gpa;
}

/**
 * unmap_sptes - Unmap shadow page table entries from the extended page table
 * @param bvma bvma data structure
 * @param hva hva start address
 * @param size total size
 * @param tlb_flush_force force to flush TLB?
 *
 * @retval true: using APIC
 * @retval false: not using APIC
 */
static bool unmap_sptes(struct emp_mm *bvma, unsigned long hva,
		        unsigned long size, bool *tlb_flush_force)
{
	int tlb_flush_needed, idx;
	struct kvm *kvm = bvma->ekvm.kvm;
	bool apic_page = false;

	//unmapping hva
	idx = srcu_read_lock(&bvma->ekvm.kvm->srcu);
	tlb_flush_needed = kvm_emp_kvm_unmap_hva_range(kvm, hva, hva + size);
	srcu_read_unlock(&bvma->ekvm.kvm->srcu, idx);

	//tlb invalidation
	if (tlb_flush_needed || *tlb_flush_force) {
		tlb_flush_needed |= get_kvm_tlbs_dirty_or_0(kvm);
		if (tlb_flush_needed) {
			bool flushed;
			flushed = flush_remote_tlbs(bvma, false);
			if (flushed)
				*tlb_flush_force = false;
		}

		if ((size == PAGE_SIZE) &&
				unlikely(hva == bvma->ekvm.apic_base_hva)) {
			make_all_cpus_req(bvma->ekvm.kvm, KVM_REQ_APIC_PAGE_RELOAD);
			apic_page = true;
		} else if (unlikely((bvma->ekvm.apic_base_hva >= hva) &&
					(bvma->ekvm.apic_base_hva < (hva + size)))) {
			make_all_cpus_req(bvma->ekvm.kvm, KVM_REQ_APIC_PAGE_RELOAD);
			apic_page = true;
		}
	}

	return apic_page;
}
#endif /* CONFIG_EMP_VM */

static inline u64 COMPILER_DEBUG
__get_sb_hva_base(struct vm_area_struct *vma, struct emp_gpa *head,u64 head_hva,
		  struct emp_gpa *sb_head, int *sb_pages_len, int *sb_offset)
{
	u64 sb_hva;
	int sb_dist = sb_head - head;
	int sb_page_order = gpa_subblock_page_order(sb_head);

	sb_hva = head_hva + (sb_dist << sb_page_order);

#ifdef CONFIG_EMP_USER
	if (unlikely(is_gpa_flags_set(sb_head, GPA_PARTIAL_MAP_MASK))) {
		if (sb_hva < vma->vm_start) {
			*sb_offset = ((u64)vma->vm_start - sb_hva) >> PAGE_SHIFT;
			*sb_pages_len = gpa_subblock_size(sb_head) - *sb_offset;
			sb_hva = (u64)vma->vm_start;
		}
		if ((sb_hva + (*sb_pages_len << PAGE_SHIFT)) > vma->vm_end) {
			u64 hva_dist = (u64)vma->vm_end - sb_hva;
			*sb_pages_len = (hva_dist >> PAGE_SHIFT);
		}
	}
#endif

	return sb_hva;
}

static inline int COMPILER_DEBUG
__get_sb_pages(struct vm_area_struct *vma, struct emp_gpa *head,
	       u64 head_hva, struct emp_gpa *sb_head)
{
#ifdef CONFIG_EMP_USER
	u64 sb_hva;
	int sb_dist, sb_pages_len;

	if (likely(!is_gpa_flags_set(sb_head, GPA_PARTIAL_MAP_MASK)))
		return gpa_subblock_size(sb_head);

	sb_dist = sb_head - head;
	sb_hva = head_hva + (sb_dist << PAGE_SHIFT);

	sb_pages_len = gpa_subblock_size(sb_head);

	if (sb_hva < (u64)vma->vm_start) {
		int not_mapped_pages = (((u64)vma->vm_start - sb_hva) >> PAGE_SHIFT);
                sb_pages_len = gpa_subblock_size(sb_head) - not_mapped_pages;
		sb_hva = (u64)vma->vm_start;
        }
	if ((sb_hva + (sb_pages_len << PAGE_SHIFT)) > vma->vm_end) {
		sb_pages_len = (vma->vm_end - sb_hva) >> PAGE_SHIFT;
	}

	return sb_pages_len;
#else /* !CONFIG_EMP_USER */
	return gpa_subblock_size(sb_head);
#endif
}

/**
 * __unmap_ptes - Unmap page table entries from the host page table
 * @param bvma bvma data structure
 * @param head head of the block
 * @param head_hva host virtual address of the head of block
 * @param pmd page middle directory (upper entry of pte)
 * @param ptl spinlock info
 * @param tlb TLB info
 *
 * @return is it dirty block?
 */
static void COMPILER_DEBUG
__unmap_ptes(struct emp_vmr *vmr, struct emp_gpa *head, unsigned long head_hva,
					pmd_t *pmd, struct mmu_gather *tlb)
{
	struct emp_gpa *gpa;
	struct emp_mm *emm = vmr->emm;
	struct mm_struct *mm = vmr->host_mm;
	struct vm_area_struct *vma = vmr->host_vma;
	unsigned long hva, addr, pfn;
	unsigned int pages_len, page_off;
	bool mapped, accessed, dirty;
	bool owned;
	int i;

	/* @hva is the address of the subblock itself; @page_off is where the
	 * pages this vmr maps start inside it. A partial map holds a single
	 * subblock, so one offset covers the loop below. */
	____local_gpa_to_hva_len_off(vmr, head, hva, pages_len, page_off);
	hva += (unsigned long) page_off << PAGE_SHIFT;

	// block-grained dirty management. need to more finer?
	dirty = false;

	for_each_gpas(gpa, head) {
		pte_t *ptep, pte, *ptep_base;
		int pte_clear_count;
		struct page *sb_page, *map_page, *page;

		/* @sb_page is the head: the reference count and the page flags
		 * live there. @map_page is the first page this vmr's ptes
		 * actually map, which is where the rmap walk starts. */
		sb_page = gpa->local_page->page;
		map_page = sb_page + page_off;
		/* Fully-unmapped shortcut. Only for a subblock every mapper sees
		 * whole: there, one page's map count of zero means nobody maps
		 * the subblock, so this vmr is the sole owner and its record can
		 * simply be retired. A PARTIAL subblock may be viewed by two
		 * vmrs over disjoint pages, so a zero on THIS vmr's first page
		 * says nothing about the other's -- taking the shortcut there
		 * would assert sole ownership falsely. The normal walk below
		 * handles it: zapped ptes are skipped one by one and only this
		 * vmr's record is removed. page_mapped() is folio-wide, so it
		 * only errs toward that walk; page_mapcount() went in 6.11. */
		if (!page_mapped(map_page)
			&& !__is_gpa_flags_set(gpa, GPA_PARTIAL_MAP_MASK)) {
			/* the pop retains @vmr as the owner */
			emp_lp_remove_pmd(emm, gpa->local_page, vmr);
			debug_lru_del_vmr_id_mark(gpa->local_page, vmr->debug_id);
			debug_assert(EMP_LP_PMDS_EMPTY(&gpa->local_page->pmds));

			/* NOTE: RSS is not changed. @vmr is the only vmr and it is the owner. */
			goto next;
		}

		mapped = false;
		accessed = false;


		/* block is aligned */
		debug_assert(emp_lp_lookup_pmd(gpa, vmr) == pmd
				|| emp_lp_lookup_pmd(gpa, vmr) == NULL);
		owned = (emp_lp_owner(gpa->local_page) == vmr);
		if (emp_lp_remove_pmd(emm, gpa->local_page, vmr) == false)
			goto next;
		debug_lru_del_vmr_id_mark(gpa->local_page, vmr->debug_id);
		if (!owned)
			/* a non-owner mapper releases its own charge */
			emp_update_rss_sub(vmr, pages_len,
						DEBUG_RSS_SUB_UNMAP_PTES,
						gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
		else if (emp_lp_owner(gpa->local_page) != vmr)
			/* the pop promoted a mapped survivor: the owner charge
			 * moves to it -- the survivor's own mapping charge
			 * becomes its owner charge, so only @vmr releases */
			emp_update_rss_sub(vmr, pages_len,
						DEBUG_RSS_SUB_UNMAP_PTES,
						gpa, DEBUG_UPDATE_RSS_SUBBLOCK);

		ptep = emp_pte_map(pmd, hva);
		ptep_base = ptep; // to emp_pte_unmap() later.
		pfn = pte_pfn(*ptep);

		pte_clear_count = 0;
		for (i = 0, addr = hva, page = map_page; i < pages_len;
				i++, addr += PAGE_SIZE, ptep++, pfn++, page++) {
			pte = *ptep;
			/* kernel may be closing vma and concurrently unmap pte.
			 * Then, just skip the unmap. */
			if (unlikely(pte_pfn(pte) == 0UL))
				continue;
#ifdef CONFIG_EMP_DEBUG
			if (unlikely(pte_pfn(pte) != page_to_pfn(page))) {
				printk(KERN_ERR "%s ERROR: page is not ours. "
					"addr: %016lx "
					"idx: %d pte: %016lx pfn: %lx "
					"page: %016lx pfn: %lx flag: %016lx "
					"page[%d]: %016lx pfn: %lx flag: %016lx "
					"vm_start: %016lx vm_end: %016lx "
					"vm_flag: %016lx vm_base: %016lx\n",
					__func__,
					addr,
					i, pte_val(pte), pte_pfn(pte),
					(unsigned long) map_page, page_to_pfn(map_page),
					map_page->flags,
					i, (unsigned long) (map_page + i),
					page_to_pfn(map_page + i), (map_page + i)->flags,
					vmr->vm_start, vmr->vm_end,
					vma->vm_flags,
					vmr->descs->vm_base);
			}
#endif
			native_pte_clear(NULL, 0, ptep);
			flush_cache_page(vma, addr, pfn);
			/* THKIM: Temporally remove the code for TLB */
			tlb_remove_tlb_entry(tlb, ptep, addr);
			pte_clear_count++;

			if (!mapped && pte_accessible(mm, pte))
				mapped = true;
			if (!accessed && pte_young(pte))
				accessed = true;
			if (!dirty && pte_dirty(pte))
				dirty = true;
			kernel_page_remove_rmap(page, vma, false);
		}

		emp_pte_unmap(ptep_base);

		page_ref_sub(sb_page, pte_clear_count);
		gpa->local_page->page_map_count -= pte_clear_count;
		debug_page_ref_mark(vmr->debug_id, gpa->local_page, -pte_clear_count);
		debug_check_lessthan(page_count(sb_page), 1);
		if (mapped && accessed &&
				!PageReferenced(sb_page)) {
			SetPageReferenced(gpa->local_page->page);
		}

next:
		hva += pages_len * PAGE_SIZE;
	}
	if (dirty) {
		set_gpa_flags_if_unset(head, GPA_DIRTY_MASK);
		SetPageDirty(head->local_page->page);
	}

	emp_update_rss_cached(vmr);
}

/** unmap_ptes - Unmap page table entries from the host page table
 * @param bvma bvma data structure
 * @param gpas gpas to unmap
 * @param start start address
 * @param size total size
 * @param tlb TLB info
 */
static void unmap_ptes(struct emp_mm *emm, struct emp_gpa *head,
		       unsigned long head_hva, unsigned long size)
{
	unsigned long end_hva = head_hva + size;
	struct emp_gpa *gpa;
	struct mapped_pmd *p;
	struct emp_vmr *vmr;
	pmd_t *pmd;
	spinlock_t *ptl;
	struct mmu_gather tlb;

	for_each_gpas(gpa, head) {
		while ((p = emp_lp_first_mapped_pmd(gpa->local_page)) != NULL) {
			/* each mapping names its own vmr; reclaim derives the
			 * range to unmap from the mapping itself (v10 8.2) */
			vmr = p->vmr;
			pmd = p->pmd;
			kernel_tlb_gather_mmu(&tlb, vmr->host_mm, head_hva, end_hva);
			/* NOTE: we acquire and release page table lock at block
			 *       granularity to prevent deadlock with
			 *       __unmap_max_block().
			 */
			ptl = pte_lockptr(vmr->host_mm, pmd);
			spin_lock(ptl);
			__unmap_ptes(vmr, head, head_hva, pmd, &tlb);
			spin_unlock(ptl);
			kernel_tlb_finish_mmu(&tlb, head_hva, end_hva);
		}
	}

	debug_unmap_ptes(emm, head, size);
}

/**
 * unmap_gpas - Unmap gpas
 * @param emm emm data structure
 * @param head head to unmap
 * @param tlb_flush_force force to flush TLB?
 * @param rss_update RSS update or not
 *
 * Unmap pages from page tables
 */
static void COMPILER_DEBUG
unmap_gpas(struct emp_mm *emm, struct emp_gpa *head, bool *tlb_flush_force)
{
	struct emp_gpa *g;
	unsigned long head_hva;
	unsigned long total_block_size;
	unsigned int sb_order;
	struct emp_vmr *vmr = emp_lp_owner(head->local_page);
	bool hpt_mapped = false;
#ifdef CONFIG_EMP_VM
	bool ept_mapped = false;

	debug_BUG_ON(is_gpa_flags_set(head, GPA_LOWMEM_BLOCK_MASK));

	/* check whether heads are mapped in HPT or EPT */
	ept_mapped = is_gpa_flags_set(head, GPA_EPT_MASK);
#endif /* CONFIG_EMP_VM */
	hpt_mapped = is_gpa_flags_set(head, GPA_HPT_MASK);
	clear_gpa_flags_if_set(head, GPA_nPT_MASK);

	/* addr and block size calculated. */
	sb_order = gpa_subblock_order(head);
	head_hva = GPN_OFFSET_TO_HVA(vmr, get_local_gpa_index(head), sb_order);
	total_block_size = __gpa_block_size(head, PAGE_SHIFT);

#ifdef CONFIG_EMP_VM
	/* gpa is mapped to extended page table */
	if (emm->ekvm.kvm && ept_mapped) {
		int idx = srcu_read_lock(&emm->ekvm.kvm->srcu);
		unmap_sptes(emm, head_hva, total_block_size, tlb_flush_force);
		debug_progress(head, head_hva);
		srcu_read_unlock(&emm->ekvm.kvm->srcu, idx);

		/* NOTE: unmap EPT but do not decrease RSS.
		 *       Current version does not support multiple VMRs
		 *       on a gpa for VMs. Thus, RSS is increased on alloc_and_fetch_pages()
		 *       and decreased on set_gpa_remote().
		 *       The following lines are for the future, when multiple
		 *       VMs share a EMP-managed memory region.
		 *
		 * if (!hpt_mapped && emp_lp_owner(head->local_page) != vmr)
		 *	emp_update_rss_sub_force(vmr, gpa_block_size(head),
		 *				DEBUG_RSS_SUB_UNMAP_GPAS,
		 *				head, DEBUG_UPDATE_RSS_BLOCK);
		 */
	}
#endif
	
	/* gpa is mapped to host page table */
	if (hpt_mapped) {
		/* conventional unmapping codes */
		unmap_ptes(emm, head, head_hva, total_block_size);
		debug_progress(head, head_hva);
	}

	for_each_gpas(g, head) {
		if (!PageReferenced(g->local_page->page))
			continue;
		ClearPageReferenced(g->local_page->page);
		set_gpa_flags_if_unset(g, GPA_REFERENCED_MASK);
	}

	debug_unmap_gpas(emm, head, head_hva, tlb_flush_force);
}

#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
#define GPADESC_ALLOC_AT_TABLE_SHIFT (10)
struct gpadesc_alloc_at_table {
	struct gpadesc_alloc_at alloc_at;
	unsigned long count;
};

void set_gpadesc_alloc_at(struct emp_vmr *vmr, unsigned long index, char *file, int line)
{
	unsigned long head_index;
	struct emp_gpa *head, *gpa;
	bool already_locked;
	gpa = raw_get_gpadesc(vmr, index);
	if (!gpa) // e.g., out-of-range index
		return;

	head_index = _emp_get_block_head_index(vmr, index, gpa_desc_order(gpa));
	head = get_exist_gpadesc(vmr, head_index);
	already_locked = ____emp_gpa_is_locked(head);
	if (!already_locked)
		__emp_lock_block(head);
	if (head->alloc_at.file == NULL) {
		head->alloc_at.file = file;
		head->alloc_at.line = line;
	}
	if (!already_locked)
		emp_unlock_block(head);
}

static inline void
gpadesc_alloc_at_insert(struct gpadesc_alloc_at *alloc_at, struct gpadesc_alloc_at_table *table)
{
	unsigned long index, num_try = 0;
	unsigned long size = 1UL << GPADESC_ALLOC_AT_TABLE_SHIFT;
	struct gpadesc_alloc_at *entry;
	// It is rare that line numbers of new_gpadesc() are same across the
	// positions. Thus, we use the line number as the starting index.
	index = ((unsigned long) alloc_at->line) % size;

	do {
		entry = &table[index].alloc_at;
		if (entry->file == alloc_at->file
				&& entry->line == alloc_at->line) {
			table[index].count++;
			return;
		}

		if (entry->file == NULL) {
			entry->file = alloc_at->file;
			entry->line = alloc_at->line;
			table[index].count = 1;
			return;
		}

		index = (index + 1) % size;
		num_try++;
	} while (likely(num_try < size));

	printk(KERN_ERR "%s: table is full. file: %s line: %d\n",
				__func__, alloc_at->file, alloc_at->line);
}

static void
gpadesc_alloc_at_show(int emm_id, int vmr_id, struct gpadesc_region *region,
					struct gpadesc_alloc_at_table *table)
{
	unsigned long start = region->start, end = region->end;
	unsigned long index;
	unsigned long size = 1UL << GPADESC_ALLOC_AT_TABLE_SHIFT;
	for (index = 0; index < size; index++) {
		if (table[index].count == 0)
			continue;
		printk(KERN_ERR "%s: emm: %d vmr: %d range: %10lx ~ %10lx table[%04ld] file: %s line: %4d count: %ld\n",
					__func__,
					emm_id, vmr_id, start, end,
					index,
					table[index].alloc_at.file,
					table[index].alloc_at.line,
					table[index].count);
	}
}
#endif

/* gpa_dir MUST be stable. */
static inline void __lock_max_block(struct emp_gpa *max_head, int num)
{
	int i;
	struct emp_gpa *gpa;
	/* lock all subblocks */
	for (i = 0, gpa = max_head; i < num; i++, gpa++)
		__emp_lock_block(gpa);
}

/* return true if any pages are dirty. return false otherwise. */
static inline bool
__unmap_subblock_single_vmr(struct emp_vmr *vmr, struct emp_gpa *gpa,
			unsigned long hva, unsigned int offset,
			unsigned long page_len, pmd_t *pmd)
{
	pte_t *ptep, pte, *ptep_base;
	bool dirty = false;
	struct page *page;
	unsigned long pfn, i;
	struct vm_area_struct *vma = vmr->host_vma;

	debug_progress(gpa, vmr->debug_id);
	debug_lru_progress_mark(gpa->local_page, vmr->debug_id);

#ifdef CONFIG_EMP_VM
	debug_BUG_ON(is_gpa_flags_set(gpa, GPA_LOWMEM_BLOCK_MASK));
#endif

	ptep = emp_pte_map(pmd, hva);
	ptep_base = ptep;
	pfn = pte_pfn(*ptep);
	/* the first page the ptes at @hva map, which is not the head of the
	 * allocation when the subblock is clipped at its head */
	page = gpa->local_page->page + offset;
	for (i = 0; i < page_len; i++, hva += PAGE_SIZE, ptep++, pfn++, page++) {
		/* kernel may be unmap this PTE due to the splitted vma
		 * Then, just skip the unmap. */
		pte = *ptep;
		if (unlikely(pte_pfn(pte) == 0UL))
			continue;
		debug_BUG_ON(pte_pfn(pte) != page_to_pfn(page));
		native_pte_clear(NULL, 0, ptep);
		flush_cache_page(vma, hva, pfn);
		tlb_remove_tlb_entry((&vmr->close_tlb), ptep, hva);
		if (!dirty && pte_dirty(pte))
			dirty = true;
		kernel_page_remove_rmap(page, vma, false);
	}

	emp_pte_unmap(ptep_base);

	/* We do not use wrapper __emp_put_pages_map(),
	 * since __put_local_page_pmd() will sync the page ref for debug */
	____emp_put_pages_map(gpa, page_len);
	return dirty;
}

static inline void
__unmap_max_block(struct emp_vmr *vmr, struct emp_gpa *max_head,
				unsigned long max_head_idx, int size)
{
	unsigned long i, head_idx, head_hva, sb_page_len;
	struct emp_gpa *head, *gpa;
	spinlock_t *ptl;
	pmd_t *pmd;
	bool dirty;

	for (i = 0, head = max_head; i < size;
			i += num_subblock_in_block(head),
			head += num_subblock_in_block(head)) {
		if (head->r_state != GPA_ACTIVE)
			continue;
		debug_assert(head->local_page);
		pmd = emp_lp_lookup_pmd(head, vmr);
		if (!pmd)
			continue;

#ifdef CONFIG_EMP_VM
		/* DO NOT unmap the low memory region for VMs */
		if (unlikely(is_gpa_flags_set(head, GPA_LOWMEM_BLOCK_MASK)))
			continue;
#endif

		/* NOTE: we acquire and release page table lock at block
		 *       granularity to prevent deadlock with __unmap_ptes().
		 */
		ptl = pte_lockptr(vmr->host_mm, pmd);
		spin_lock(ptl);

		head_idx = max_head_idx + i;
		head_hva = GPN_OFFSET_TO_HVA(vmr, head_idx, gpa_subblock_order(head));
#ifdef CONFIG_EMP_USER
		if (unlikely(is_gpa_flags_set(head, GPA_PARTIAL_MAP_MASK))) {
			unsigned int sb_page_off;
			____partial_gpa_len_off(vmr, head, head_idx, head_hva,
						sb_page_len, sb_page_off);
			dirty = __unmap_subblock_single_vmr(vmr, head,
					head_hva + ((unsigned long) sb_page_off
							<< PAGE_SHIFT),
					sb_page_off, sb_page_len, pmd);
			if (dirty)
				set_gpa_flags_if_unset(head, GPA_DIRTY_MASK);
			emp_update_rss_sub(vmr, sb_page_len,
					DEBUG_RSS_SUB_UNMAP_MAX_BLOCK_PARTIAL,
					head, DEBUG_UPDATE_RSS_BLOCK);
			spin_unlock(ptl);
			continue;
		}
#endif

		dirty = false;
		sb_page_len = gpa_subblock_size(head);
		for_each_gpas(gpa, head) {
#ifdef CONFIG_EMP_DEBUG
			/* block is aligned */
			debug_assert(gpa == head ||
					emp_lp_lookup_pmd(gpa, vmr) == pmd);
#endif
			dirty |= __unmap_subblock_single_vmr(vmr, gpa, head_hva,
							0, sb_page_len, pmd);
			head_hva += sb_page_len << PAGE_SHIFT;
		}
		if (dirty)
			set_gpa_flags_if_unset(head, GPA_DIRTY_MASK);
		emp_update_rss_sub(vmr, gpa_block_size(head),
					DEBUG_RSS_SUB_UNMAP_MAX_BLOCK,
					head, DEBUG_UPDATE_RSS_BLOCK);

		spin_unlock(ptl);
	}
}

static inline void
__put_local_page_pmd(struct emp_vmr *vmr, struct emp_gpa *gpa)
{
	bool owned = (emp_lp_owner(gpa->local_page) == vmr);
	int removed = 0;
	if (emp_lp_remove_pmd(vmr->emm, gpa->local_page, vmr)) {
		removed = 1;
		debug_lru_del_vmr_id_mark(gpa->local_page, vmr->debug_id);
		debug_page_ref_unmap_end(gpa->local_page);
		debug_page_ref_mark(vmr->debug_id, gpa->local_page, -1);
	}
	if (owned) {
		/* The pop promoted a mapped survivor, or retained @vmr as an
		 * unmapped owner. This is close: a retained representative is
		 * cleared here, and only here (v10 10.4). A promoted survivor
		 * keeps its own mapping charge as the owner charge, so no
		 * transfer is needed; an owner without a mapping releases. */
		if (emp_lp_owner(gpa->local_page) == vmr)
			emp_lp_clear_owner(gpa->local_page);
		debug_lru_set_vmr_id_mark(gpa->local_page,
				emp_vmr_dbgid(emp_lp_owner(gpa->local_page)));
		if (!removed)
			emp_update_rss_sub(vmr,
					__local_gpa_to_page_len(vmr, gpa),
					DEBUG_RSS_SUB_PUT_LOCAL_PAGE,
					gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
	}
	if (emp_lp_count_pmd(gpa->local_page) == 0) {
		clear_gpa_flags_if_set(gpa, GPA_nPT_MASK);
		if (PageReferenced(gpa->local_page->page)) {
			ClearPageReferenced(gpa->local_page->page);
#ifndef CONFIG_EMP_DEBUG_TRIGGER_REDUCE
			set_gpa_flags_if_unset(gpa, GPA_REFERENCED_MASK);
#endif
		}
	}
}

/* remove the vmr's record and pmd if inserted.
 * return head's refcnt.
 */
static inline int
__put_max_block(struct emp_mm *emm, struct vcpu_var *cpu,
		struct emp_vmr *vmr, int vm_refcnt, bool may_dirty,
		struct emp_gpa *max_head, unsigned long size,
		struct emp_gpa **gpa_dir, unsigned long max_head_idx)
{
	int refcnt = atomic_read(&max_head->refcnt)
				- ((vm_refcnt == 0) ? 1 : 0);
	unsigned long i;
	struct emp_gpa *head, *gpa;

	for (i = 0, head = max_head; i < size;
			i += num_subblock_in_block(head),
			head += num_subblock_in_block(head)) {
		unsigned long gpa_idx = max_head_idx + i;
		debug_progress(head, (((u64) vmr->debug_id) << 32) | head->r_state);
		if (may_dirty)
			set_gpa_flags_if_unset(head, GPA_DIRTY_MASK);
		for_each_gpas(gpa, head) {
			if (gpa->local_page)
				__put_local_page_pmd(vmr, gpa);
			/* If vm_refcnt > 0, this is MAP_SHARED and there is still other vmrs.
			 * Don't decrement gpa's reference count for such case.
			 */
			if (vm_refcnt == 0)
				remove_gpa_dir(vmr, gpa_dir, gpa_idx);
#ifdef CONFIG_EMP_DEBUG
			refcnt = atomic_read(&gpa->refcnt);
			if (refcnt != atomic_read(&max_head->refcnt))
				BUG();
#endif
			gpa_idx++;
		}

		switch (head->r_state) {
		case GPA_INIT:
		case GPA_WB:
				/* gpa on GPA_WB and GPA_INIT will be cleared later. */
				continue;
		case GPA_INACTIVE:
				if (emp_lp_owner(head->local_page))
					/* another owner remains: keep it */
					continue;
				break;
		case GPA_ACTIVE:
				if (head->local_page->num_pmds > 0)
					/* This is ACTIVE and mapped to other process. */
					continue;
				break;
#ifdef CONFIG_EMP_DEBUG
		default:
				BUG();
#endif
		}

		debug_lru_progress_mark(head->local_page, head->r_state);
		debug_lru_progress_mark(head->local_page, __get_gpa_flags(head));
		debug_lru_progress_mark(head->local_page, head->local_page->flags);
		/* For GPA_ACTIVE and GPA_INACTIVE, remove from the list */
#ifdef CONFIG_EMP_EXT
		emp_ops.remove_gpa_from_lru(emm, head);
#else
		remove_gpa_from_lru(emm, head);
#endif

		debug_lru_progress_mark(head->local_page, __get_gpa_flags(head));
		debug_lru_progress_mark(head->local_page, head->local_page->flags);
		debug_lru_progress_mark(head->local_page, refcnt);
		debug_lru_progress_mark(head->local_page, vm_refcnt);
#ifdef CONFIG_EMP_USER
		if (refcnt + vm_refcnt == 0)
			/* This solely belongs to the closing vmr.
			 * Just clear it. */
			continue;

		/* @head has some owner but we don't know who is.
		 * Move it to writeback lists which does not require the owner.
		 */

#ifdef CONFIG_EMP_EXT
		emp_ops.emp_writeback_block(emm, head, cpu);
#else
		emp_writeback_block(emm, head, cpu);
#endif
		if (head->r_state == GPA_WB)
			add_inactive_list_page_len(emm, head);
#endif /* CONFIG_EMP_USER */
	}

	debug_assert(refcnt >= 0);
	return refcnt + vm_refcnt;
}

#ifdef CONFIG_EMP_BLOCK
static inline void
__wait_for_prefetch_max_block(struct emp_mm *emm, struct vcpu_var *cpu,
				struct emp_gpa *max_head, unsigned long size)
{
	unsigned long i;
	struct emp_gpa *gpa;
	for (i = 0, gpa = max_head; i < size;
			i += num_subblock_in_block(gpa),
			gpa += num_subblock_in_block(gpa)) {
		if (!is_gpa_flags_set(gpa, GPA_PREFETCHED_MASK))
			continue;
		wait_for_prefetched_block(emm, cpu, gpa);
		/* TODO: During unmap vma, calling sync_hpt_map_in_block() is
		 *       NOT theoretically required. But, the following codes
		 *       assume that hpt map in block is synchronized.*/
		sync_hpt_map_in_block(emm, gpa, false);
		debug_check_sync_hpt(emm, gpa, NULL, DEBUG_SYNC_HPT_AT_UNMAP);
	}
}
#endif /* CONFIG_EMP_BLOCK */

static void flush_gpa(struct emp_vmr *vmr, struct vcpu_var *cpu,
	struct emp_gpa *max_head, unsigned long head_idx, unsigned long size)
{
	unsigned long i;
	struct emp_mm *emm = vmr->emm;
	struct emp_gpa *head, *gpa;
	debug_assert(atomic_read(&max_head->refcnt) == 0);
	for (i = 0, head = max_head; i < size;
			i += num_subblock_in_block(head),
			head += num_subblock_in_block(head)) {
		if (head->r_state == GPA_ACTIVE || head->r_state == GPA_INACTIVE) {
			debug_assert(head->local_page);
			// Kernel already cleared PTEs for ACTIVE gpas
			for_each_gpas_reverse(gpa, head)
				emm->vops.free_gpa(emm, gpa, NULL);
			head->r_state = GPA_INIT;
		} else if (head->r_state == GPA_WB) {
			struct vcpu_var *v;
			debug_assert(head->local_page);
			v = emp_get_vcpu_from_id(emm, head->local_page->cpu);
			debug_progress(head->local_page->w, head);
			emm->sops.clear_writeback_block(emm, head,
							head->local_page->w,
							v, true, true);
			for_each_gpas_reverse(gpa, head)
				emm->vops.free_gpa(emm, gpa, NULL);
			head->r_state = GPA_INIT;
		}
	}
}

/* gpa_dir MUST be stable. */
static inline void
__unlock_max_block(struct emp_gpa *max_head, unsigned long num)
{
	unsigned long i;
	struct emp_gpa *gpa;
	/* unlock all subblocks */
#ifdef CONFIG_EMP_DEBUG
	for (i = 0, gpa = max_head + (num - 1); i < num; i++, gpa--) {
		if (gpa == emp_get_block_head(gpa))
			/* for head, we should check more things */
			emp_unlock_block(gpa);
		else
			__emp_unlock_block(gpa);
	}
#else
	for (i = 0, gpa = max_head + (num - 1); i < num; i++, gpa--)
		__emp_unlock_block(gpa);
#endif
}

/* desc_refcnt: after decrement
 * [idx_start, idx_end): the WALK range -- the closing vmr's view, where its
 *                       mapping records must be stripped; clipped to the
 *                       region on the region's own block grid
 * [unc_start, unc_end): the RELEASE range -- subblocks no surviving view
 *                       covers; empty means strip only
 * perblock_check:       the uncovered remainder was not one range; decide
 *                       release per block by querying the views
 */
static unsigned long
free_gpa_dir_region(struct emp_vmr *vmr, struct vcpu_var *cpu,
		struct emp_gpa **gpa_dir, struct gpadesc_region *region,
		int vm_refcnt, bool do_unmap,
		unsigned long idx_start, unsigned long idx_end,
		unsigned long unc_start, unsigned long unc_end,
		bool perblock_check)
{
	struct emp_mm *emm = vmr->emm;
	unsigned long num_allocated = 0;
	struct emp_gpa *max_head;
	unsigned long i, step, start, end;
	int put_refcnt;
	int sb_order = bvma_subblock_order(emm);
	int desc_order = region->alloc_order - bvma_subblock_order(emm);
	struct kmem_cache *cachep = get_gpadesc_alloc(emm, desc_order);
	bool may_dirty = false;
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	struct gpadesc_alloc_at_table *table;
	table = emp_vzalloc(sizeof(struct gpadesc_alloc_at_table)
					* (1 << GPADESC_ALLOC_AT_TABLE_SHIFT));
#endif
	if (!do_unmap && vm_refcnt > 0) {
		/* if do_unmap == true, we will check dirty bit of PTE */
		unsigned long vm_flags = vmr->host_vma->vm_flags;
		if ((vm_flags & (VM_SHARED | VM_MAYSHARE)) != 0
				&& (vm_flags & (VM_WRITE | VM_MAYWRITE)) != 0)
			may_dirty = true;
	}

	step = 1UL << desc_order;
	/* Clip the walk to [idx_start, idx_end) on this region's own block
	 * grid: blocks align from region->start (block_aligned_start for the
	 * main region), not from index zero, so an absolute mask could start
	 * the scan mid-group and mistake a member descriptor for a head.
	 * Rounding OUT keeps a block shared with a neighboring view visited:
	 * nothing is released for it while a view covers it, and only this
	 * vmr's own mapping is removed. */
	if (idx_end <= region->start || idx_start >= region->end)
		return 0;
	start = idx_start > region->start
		? region->start + ((idx_start - region->start) & ~(step - 1))
		: region->start;
	end = idx_end < region->end
		? region->start + (((idx_end - region->start) + step - 1)
					& ~(step - 1))
		: region->end;

	// Don't use for_all_gpa_heads_range() here. desc_order may be different
	// from gpa_block_order(head) due to elastic block.
	for (i = start; i < end; i += step) {
		if (!gpa_dir[i])
			continue;
		max_head = gpa_dir[i];

		/* Every block in the walk is processed -- the strip of this
		 * vmr's mapping records is unconditional. Coverage decides
		 * only whether the slots may be RELEASED: __put_max_block()
		 * releases when its refcnt argument is zero. */
		put_refcnt = vm_refcnt;
#ifdef CONFIG_EMP_USER
		if (vm_refcnt > 0) {
			if (perblock_check) {
				/* view coordinates are vmdesc-relative pages */
				if (!emp_vmdesc_view_is_covered(vmr->descs,
							i << sb_order,
							(i + step) << sb_order))
					put_refcnt = 0;
			} else if (i >= unc_start && i + step <= unc_end) {
				/* wholly inside the uncovered remainder; a
				 * block straddling its edge stays kept, which
				 * is conservative and cleaned by a later or
				 * the final close */
				put_refcnt = 0;
			}
		}
#endif
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
		gpadesc_alloc_at_insert(&max_head->alloc_at, table);
#endif
		__lock_max_block(max_head, step);
		debug_free_gpa_dir_region(max_head, desc_order);
#ifdef CONFIG_EMP_BLOCK
		__wait_for_prefetch_max_block(emm, cpu, max_head, step);
#endif
		if (do_unmap)
			__unmap_max_block(vmr, max_head, i, step);
#ifdef CONFIG_EMP_DEBUG_RSS
		else {
			/* if kernel unmaps page table entries,
			 * it also decreases RSS.
			 */
			int __i;
			struct emp_gpa *__head;
			for (__i = 0, __head = max_head; __i < step;
					__i += num_subblock_in_block(__head),
					__head += num_subblock_in_block(__head)) {
				if (__head->r_state != GPA_ACTIVE)
					continue;
				debug_assert(__head->local_page);
				if (!emp_lp_lookup_vmr(__head, vmr))
					continue;
				emp_update_rss_sub_kernel(vmr,
					__local_block_to_page_len(vmr, __head),
					DEBUG_RSS_SUB_KERNEL_FREE_GPA_DIR,
					__head, DEBUG_UPDATE_RSS_BLOCK);
			}
		}
#endif
		if (__put_max_block(emm, cpu, vmr, put_refcnt, may_dirty,
					max_head, step,
					gpa_dir, i) > 0) {
			__unlock_max_block(max_head, step);
			continue;
		}

#ifdef CONFIG_EMP_EXT
		if (emp_ext.flush_gpa)
			emp_ext.flush_gpa(vmr, cpu, max_head, i, step);
#endif
		flush_gpa(vmr, cpu, max_head, i, step);
		remote_page_release(emm, max_head, step);
		emp_kmem_cache_free(cachep, max_head);
		num_allocated += step;
	}
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	gpadesc_alloc_at_show(emm->id, vmr->debug_id, region, table);
	emp_vfree(table);
#endif

	emp_update_rss_cached(vmr);

	return num_allocated;
}

/**
 * close_and_free_gpas - Close and free gpas
 * @param vmr emp_vmr data structure
 * @param do_unmap if true, we need to unmap page tagle. if false, kernel already did unmap.
 *
 * Free the gpa data structure for the pages
 */
static int close_and_free_gpas(struct emp_vmr *vmr, bool do_unmap)
{
	int r;
	unsigned long num_allocated = 0;
	struct emp_mm *emm = vmr->emm;
	struct vcpu_var *cpu;
	struct emp_vmdesc *desc = vmr->descs;
	int vm_refcnt;
	unsigned long idx_start = 0, idx_end = desc->gpa_len;
	unsigned long unc_start = 0, unc_end = 0; /* empty: release nothing */
	bool perblock_check = false;

	if (unlikely(!desc->gpa_dir_alloc))
		return 0;
	
#ifdef CONFIG_EMP_USER
	if (atomic_cmpxchg(&desc->is_closing, 0, 1) != 0)
		/* The other thread is closing this vmdesc. Wait for it. */
		/* Uninterruptibly: two vmrs sharing a vmdesc are usually torn
		 * down by a dying process group, so every closer has a fatal
		 * signal pending; an interruptible wait returns at once and
		 * both closers walk the one gpa_dir together, one of them
		 * freeing blocks the other still maps. It cannot hang: a
		 * waiter still holds a vmr referencing this vmdesc, so the
		 * holder always takes the vm_refcnt > 0 path, which clears
		 * is_closing and wakes this queue. */
		wait_event(desc->closing_wq,
				atomic_cmpxchg(&desc->is_closing, 0, 1) == 0);
#endif
	cpu = emp_this_cpu_ptr(emm->pcpus);
#ifdef CONFIG_EMP_VM
	if (emm->ekvm.kvm) {
		/* This locking can be long, but VM is terminating. In addition,
		 * there is no shared mapping for VM. Thus, free_gpa_dir_region()
		 * will not request I/O for writeback.
		 * => No scheduling (due to I/O) in atomic. */
		lock_kvm_mmu_lock(emm->ekvm.kvm);
	}
#endif /* CONFIG_EMP_VM */

#ifdef CONFIG_EMP_USER
	/* Drop this vmr's view of the namespace before deciding what is left.
	 * The lifetime reference is dropped in the same breath, so a concurrent
	 * checker sees refcount == sum(view counts) at every stable boundary. */
	emp_vmdesc_view_del(desc, vmr_view_start(vmr), vmr_view_end(vmr));
	vm_refcnt = atomic_dec_return(&desc->refcount);
	if (vm_refcnt > 0) { // vm_refcnt > 0
		bool fine;
		unsigned long addr_start, addr_end;

		/* The WALK range and the RELEASE range are different things.
		 *
		 * The walk must cover this vmr's whole view: the vmr's own
		 * mapped-pmd records and representative ownership live on
		 * blocks in that range and must be stripped before the vmr is
		 * freed, whatever the coverage says -- a covered block skipped
		 * here keeps a raw pointer to a vmr about to be released, and
		 * the next consumer of that stale pointer (els, reclaim owner
		 * reads) dereferences freed memory.
		 *
		 * Release is decided per block, from the surviving views:
		 * inside the uncovered remainder when one range describes it,
		 * or by a per-block coverage query when it does not. Fully
		 * covered means release nothing -- but still walk. */
		idx_start = vmr_view_start(vmr) >> desc->subblock_order;
		idx_end = (vmr_view_end(vmr) + bvma_subblock_size(emm) - 1)
						>> desc->subblock_order;

		addr_start = vmr_view_start(vmr);
		addr_end = vmr_view_end(vmr);
		fine = emp_vmdesc_view_find_uncovered(desc, &addr_start, &addr_end);
		if (fine && addr_start < addr_end) {
			/* release only whole subblocks inside the uncovered
			 * range: round inward */
			unc_start = (addr_start + bvma_subblock_size(emm) - 1)
						>> desc->subblock_order;
			unc_end = addr_end >> desc->subblock_order;
		} else if (!fine)
			perblock_check = true;
	}
#else /* !CONFIG_EMP_USER */
	vm_refcnt = 0;
#endif /* !CONFIG_EMP_USER */

	if (do_unmap)
		dprintk("[DEBUG] %s: UNMAP emm: %d vmr: %d mm: %lx vma: %lx "
			"vm_base: 0x%lx size: 0x%lx vm_start: 0x%lx vm_end: 0x%lx\n",
			__func__, emm->id, emp_vmr_dbgid(vmr),
			(unsigned long) vmr->host_mm, (unsigned long) vmr->host_vma,
			vmr->descs->vm_base,
			vmr->descs->gpa_len << (vmr->descs->subblock_order + PAGE_SHIFT),
			vmr->vm_start, vmr->vm_end);

	if (do_unmap)
		kernel_tlb_gather_mmu(&vmr->close_tlb, vmr->host_mm,
					vmr->vm_start, vmr->vm_end);

	/* if the gpas pointer is still used, free the allocated memory and
	 * save NULL */
	for (r = 0; r < desc->num_region; r++)
		num_allocated += free_gpa_dir_region(vmr, cpu, desc->gpa_dir,
						&desc->regions[r], vm_refcnt,
						do_unmap,
						idx_start, idx_end,
						unc_start, unc_end,
						perblock_check);

	if (do_unmap)
		kernel_tlb_finish_mmu(&vmr->close_tlb,
					vmr->vm_start, vmr->vm_end);


#ifdef CONFIG_EMP_VM
	if (emm->ekvm.kvm)
		unlock_kvm_mmu_lock(emm->ekvm.kvm);
#endif
	dprintk(KERN_INFO "%s: gpa descriptor allocation: %ld/%ld (%ld.%02ld%%)\n",
				__func__, num_allocated, desc->gpa_len,
				num_allocated * 100 / desc->gpa_len,
				num_allocated * 10000 / desc->gpa_len % 100);

#ifdef CONFIG_EMP_USER
	if (vm_refcnt > 0) {
		debug_assert(atomic_read(&desc->is_closing) == 1);
		atomic_set(&desc->is_closing, 0);
		/* wake_up(), not wake_up_interruptible(): the waiter sleeps
		 * in TASK_UNINTERRUPTIBLE, which an interruptible wake does
		 * not touch -- it would sleep forever on an already-true
		 * condition */
		wake_up(&desc->closing_wq);
		return vm_refcnt;
	}
#endif

	desc->gpa_dir = (struct emp_gpa **) NULL;
	emp_vfree(desc->gpa_dir_alloc);
	desc->gpa_dir_alloc = (void *) NULL;
	return 0;
}

#ifdef CONFIG_EMP_USER
/* Clear block's intermediate states before split a block to several blocks.
 * Currently, the callers are CoW and split, enabled by CONFIG_EMP_USER.
 */
void clear_block_for_reduction(struct emp_mm *emm, struct emp_vmr *vmr,
			struct emp_gpa *head, unsigned long head_idx)
{
#ifdef CONFIG_EMP_EXT
	bool check_map = false;
#endif
#ifdef CONFIG_EMP_BLOCK
	/* Support CSF and CPF
	 * - We should wait for fetching whole block and install ptes. */
	if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK)) {
#ifdef CONFIG_EMP_VM
		/* Since VM does not use fork */
		debug_assert(is_gpa_flags_set(head, GPA_EPT_MASK) == false);
#endif
		if (is_gpa_flags_set(head, GPA_HPT_MASK)) {
			clear_gpa_prefetched_hpt(emm, vmr, head, head_idx);
#ifdef CONFIG_EMP_EXT
			check_map = true;
#endif
		}
		clear_gpa_flags_if_set(head, GPA_PREFETCHED_MASK);
		emp_stat_inc(emm, csf_fault);
	}
#endif /* CONFIG_EMP_BLOCK */
#ifdef CONFIG_EMP_EXT
	if (check_map == false && emp_ext.prepare_install_hptes)
		emp_ext.prepare_install_hptes(emm, vmr, head, NULL, false, true);
#endif

/* NOTE: CONFIG_EMP_IO is enabled only with CONFIG_EMP_VM.
 *       Currently, all callers of this function are enabled when CONFIG_EMP_USER
 *       is true. Elastic block management may reduce a block with CONFIG_EMP_VM,
 *       but it reduces block at inactive list. They do not need to clear
 *       the blocks.
 *
 *       However, we remain this part for the future.
 *       Note that this part may re-lock the head.
 */
#ifdef CONFIG_EMP_IO
	// wait for completion of IO in progress
	if (is_gpa_flags_set(head, GPA_IO_IP_MASK)) {
		struct page *page;
		debug_BUG_ON(!head->local_page);
		debug_BUG_ON(!head->local_page->page);
		page = head->local_page->page;
		emp_unlock_block(head);

		wait_on_page_locked(page);

		head = emp_lock_block(vmr, NULL, head_idx);
		debug_BUG_ON(!head); // gpa has existed.
	}
#endif
}
#endif /* CONFIG_EMP_USER */


/**
 * cleanup_gpa - Reinitialize the gpa data structure
 * @param bvma bvma data structure
 * @param gpa gpa
 *
 * Clean & initialize gpa data structure for the page
 */
struct page *
cleanup_gpa(struct emp_mm *bvma, struct emp_gpa *gpa)
{
	struct local_page *local_page = gpa->local_page;
	struct page *gpa_page;

#ifdef CONFIG_EMP_EXT
	if (emp_ext.cleanup_gpa)
		emp_ext.cleanup_gpa(bvma, gpa);
#endif
	gpa->local_page = NULL;
	clear_gpa_flags_if_set(gpa, GPA_CLEANUP_MASK);
	if (local_page) {
		gpa_page = local_page->page;
		bvma->lops.free_local_page(bvma, local_page);
		gpa_page->private = 0;
		emp_clear_pg_mlocked(gpa_page);
	} else {
		gpa_page = NULL;
	}

	return gpa_page;
}

/**
 * set_gpa_remote - Set the flag to inform the page is stored in remote memory
 * @param emm emm data structure
 * @param cpu working cpu ID
 * @param g gpa
 * @param zero_page is the page zero-page?
 *
 * Do the work listed below
 * + register the page to remote memory
 * + free the page and add it to the free list
 * + set remote page flag
 */
static void COMPILER_DEBUG
set_gpa_remote(struct emp_mm *emm, struct vcpu_var *cpu, struct emp_gpa *g)
{
	debug_set_gpa_remote(emm, g);

	/* NOTE: free_gpa() clears g->local_page */
	if (likely(g->local_page) && emp_lp_owner(g->local_page)) {
		struct emp_vmr *vmr = emp_lp_owner(g->local_page);
		emp_update_rss_sub(vmr, __local_gpa_to_page_len(vmr, g),
					DEBUG_RSS_SUB_SET_REMOTE,
					g, DEBUG_UPDATE_RSS_SUBBLOCK);
	}

	emm->vops.free_gpa(emm, g, cpu);
	g->r_state = GPA_INIT;
	set_gpa_flags_if_unset(g, GPA_REMOTE_MASK);
}

#ifdef CONFIG_EMP_USER
/**
 * emp_vmdesc_view_init - initialize the vmr view metadata of a vmdesc
 * @param desc vmdesc
 *
 * The view list starts empty. Its owner registers its own interval right after
 * the backing origin (desc->vm_base) is known. Note that a vmdesc built by
 * copying another one must be re-initialized here: the copy inherits neither
 * the intervals nor the overflow entries of the original.
 */
void emp_vmdesc_view_init(struct emp_vmdesc *desc)
{
	spin_lock_init(&desc->view_lock);
	desc->views.start = 0;
	desc->views.end = 0;
	desc->views.count = 0;
	desc->views.next = NULL;
}

/**
 * __emp_vmdesc_view_add - add one vmr view of [@start, @end) to @desc
 * @param desc vmdesc
 * @param start start of the view in vmdesc-relative base page coordinates
 * @param end end of the view, exclusive
 * @param node preallocated overflow entry. Consumed and set to NULL if used.
 *             May be NULL when the caller knows no new interval shape can be
 *             required: a fork reproduces an interval which already exists,
 *             and the first vmr of a namespace finds the embedded entry free.
 *
 * @retval 0: success
 * @retval -ENOMEM: a new distinct interval was required but @node was empty
 *
 * The embedded entry is empty only while the whole list is empty, so an empty
 * embedded entry is enough to detect the first-vmr case.
 */
static int __emp_vmdesc_view_add(struct emp_vmdesc *desc, unsigned long start,
			unsigned long end, struct emp_vmdesc_view **node)
{
	struct emp_vmdesc_view *v, *prev, *n;

	debug_assert(start < end);

	if (desc->views.count == 0) {
		debug_assert(desc->views.next == NULL);
		desc->views.start = start;
		desc->views.end = end;
		desc->views.count = 1;
		return 0;
	}

	/* The list is kept sorted: ascending start, then ascending end among
	 * equal starts. An identical interval, if present, sits exactly at the
	 * insertion position, so one walk finds either. Deletion preserves the
	 * order -- removing a node keeps the sequence, and the embedded-entry
	 * promotion copies the successor, which is the minimum of what
	 * remains. find_uncovered() and is_covered() rely on the order to walk
	 * left to right and to stop at the first entry starting at or after
	 * their range end. */
	prev = NULL;
	for (v = &desc->views; v; prev = v, v = v->next) {
		if (v->start == start && v->end == end) {
			v->count++;
			return 0;
		}
		if (v->start > start
				|| (v->start == start && v->end > end))
			break;
	}

	debug_assert(node != NULL && *node != NULL);
	if (unlikely(node == NULL || *node == NULL))
		return -ENOMEM;

	n = *node;
	*node = NULL;

	if (v == &desc->views) {
		/* The new interval sorts before every existing one, but the
		 * embedded entry must stay the physical head. So the new
		 * interval goes into the embedded entry, and the embedded
		 * entry's previous content moves out into the allocated node
		 * right behind it: it was the minimum until now, so it still
		 * precedes every overflow entry and the order holds. */
		*n = desc->views;	/* start, end, count, next */
		desc->views.start = start;
		desc->views.end = end;
		desc->views.count = 1;
		desc->views.next = n;
	} else {
		n->start = start;
		n->end = end;
		n->count = 1;
		n->next = v;		/* NULL when appending at the tail */
		prev->next = n;
	}
	return 0;
}

int emp_vmdesc_view_add(struct emp_vmdesc *desc, unsigned long start,
			unsigned long end, bool first_or_shared)
{
	int ret;
	struct emp_vmdesc_view *node = NULL;

	if (!first_or_shared) {
		node = emp_kzalloc(sizeof(struct emp_vmdesc_view), GFP_KERNEL);
		if (node == NULL)
			return -ENOMEM;
	}

	spin_lock(&desc->view_lock);
	ret = __emp_vmdesc_view_add(desc, start, end, &node);
	spin_unlock(&desc->view_lock);
	if (node)
		emp_kfree(node);
	return ret;
}

/**
 * __emp_vmdesc_view_del - remove one vmr view of [@start, @end) from @desc
 * @param desc vmdesc
 * @param start start of the view in vmdesc-relative base page coordinates
 * @param end end of the view, exclusive
 *
 * Never allocates. If the embedded entry empties while overflow entries remain,
 * one of them is promoted into it and the freed node is released after the lock
 * is dropped.
 */
static struct emp_vmdesc_view *
__emp_vmdesc_view_del(struct emp_vmdesc *desc, unsigned long start,
			unsigned long end)
{
	struct emp_vmdesc_view *v, *prev = NULL, *dead = NULL;

	for (v = &desc->views; v; prev = v, v = v->next)
		if (v->count > 0 && v->start == start && v->end == end)
			break;

	if (unlikely(v == NULL)) {
		printk(KERN_ERR "%s: ERROR: no view entry for [0x%lx, 0x%lx)\n",
				__func__, start, end);
		return NULL;
	}

	if (--v->count > 0)
		return NULL;

	if (v != &desc->views) {
		prev->next = v->next;
		dead = v;
	} else if (desc->views.next) {
		/* promote an overflow entry into the embedded one */
		dead = desc->views.next;
		desc->views.start = dead->start;
		desc->views.end = dead->end;
		desc->views.count = dead->count;
		desc->views.next = dead->next;
	}
	/* else, the last view is gone and the embedded entry stays empty */
	return dead;
}

void emp_vmdesc_view_del(struct emp_vmdesc *desc, unsigned long start,
			unsigned long end)
{
	struct emp_vmdesc_view *dead;

	spin_lock(&desc->view_lock);
	dead = __emp_vmdesc_view_del(desc, start, end);
 	spin_unlock(&desc->view_lock);
 	if (dead)
 		emp_kfree(dead);
 }

/**
 * emp_vmdesc_view_split - partition one vmr view of [@start, @end) at @mid
 * @param desc vmdesc
 * @param start start of the view being split
 * @param end end of the view being split, exclusive
 * @param mid split boundary, start < mid < end
 *
 * @retval 0: success
 * @retval -ENOMEM: a new interval shape was required but its candidate entry
 *                  was empty. The view metadata is left consistent, but the
 *                  missing half's coverage is lost.
 *
 * One interval count unit becomes one unit of [start, mid) and one of
 * [mid, end) without releasing the lock, so a close running in another mm sees
 * either the whole interval or both halves and never the boundary as
 * uncovered. The union of coverage is identical before and after.
 */
int emp_vmdesc_view_split(struct emp_vmdesc *desc, unsigned long start,
			unsigned long end, unsigned long mid)
{
	struct emp_vmdesc_view *dead, *n1, *n2;
	int ret;

	debug_assert(start < mid && mid < end);

	/* One interval unit is removed and two are added, but TWO new nodes can
	 * be needed -- not one. The deletion frees a node only when the entry
	 * it decremented reached count 0, so the removed interval may not actually
	 * remove the corresponding entry.
	 */
	n1 = emp_kzalloc(sizeof(struct emp_vmdesc_view), GFP_KERNEL);
	n2 = emp_kzalloc(sizeof(struct emp_vmdesc_view), GFP_KERNEL);
	if (n1 == NULL || n2 == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock(&desc->view_lock);
	dead = __emp_vmdesc_view_del(desc, start, end);
	if (dead != NULL) { /* prefer to use earlier allocation */
		struct emp_vmdesc_view *n = n2;
		n2 = n1;
		n1 = dead;
		dead = n;
	}
	ret = __emp_vmdesc_view_add(desc, start, mid, &n1);
	if (ret == 0)
		ret = __emp_vmdesc_view_add(desc, mid, end, &n2);
	spin_unlock(&desc->view_lock);

	if (dead)
		emp_kfree(dead);
out:
	if (n1)
		emp_kfree(n1);
	if (n2)
		emp_kfree(n2);
	return ret;
}

/**
 * emp_vmdesc_view_find_uncovered - is [@start, @end) still viewed through some vmr?
 * @param desc vmdesc
 * @param *start start of the range in vmdesc-relative base page coordinates
 * @param *end end of the range, exclusive
 *
 * @retval true: the result is fine. [*start, *end) is not covered by any other
 *               views. If the whole range is covered, *start == *end.
 * @retval false: conservative -- the uncovered remainder may need multiple
 *                ranges. The caller falls back to emp_vmdesc_view_is_covered()
 *                on finer ranges, where the verdict is exact at block
 *                granularity because no block straddles a view boundary.
 *
 * Relies on the view list being sorted by ascending start (then end): the walk
 * consumes coverage left to right, and stops at the first view beginning at or
 * after the range end.
 */
bool emp_vmdesc_view_find_uncovered(struct emp_vmdesc *desc, unsigned long *__start,
			unsigned long *__end)
{
	struct emp_vmdesc_view *v;
	unsigned long start = *__start, end = *__end;
	bool fine = true;

	spin_lock(&desc->view_lock);
	for (v = &desc->views; v; v = v->next) {
		if (unlikely(v->count == 0)) /* only for initial state */
			continue;
		if (v->end <= start)
			continue;
		if (v->start >= end)
			break;	/* sorted by start: nothing later intersects */
		if (v->start <= start && v->end >= end) {
			/* fully covered */
			*__start = 0;
			*__end = 0;
			goto out;
		}
		if (start < v->start && v->end < end) {
			/* v splits the remainder in two */
			fine = false;
			goto out;
		}

		/* A one-sided overlap: reduce the bound v anchors, and only
		 * that one. Updating both bounds at once would compute the gap
		 * between v's own edges, which is inverted by construction --
		 * a left overlap would return "fully covered" while the right
		 * part of the range has no view at all. */
		if (v->start <= start) {
			/* left: v->end < end, or the full-cover case above
			 * would have taken it */
			start = v->end;
			debug_assert(start < end);
		} else {
			/* right: v->start > start, or the left case would
			 * have taken it. Sorted by start, every later view
			 * begins at or beyond the new end. */
			end = v->start;
			debug_assert(start < end);
			break;
		}
	}
	*__start = start;
	*__end = end;
out:
	spin_unlock(&desc->view_lock);
	return fine;
}

/**
 * emp_vmdesc_view_is_covered - does any live view intersect [@start, @end)?
 * @param desc vmdesc
 * @param start start of the range in vmdesc-relative base page coordinates
 * @param end end of the range, exclusive
 *
 * @retval true: at least one live view intersects the range
 * @retval false: no live view touches it
 *
 * The conservative fallback for emp_vmdesc_view_find_uncovered(): when the
 * uncovered remainder cannot be represented as one range, the caller asks this
 * about a smaller range and keeps whatever intersects. At block granularity
 * the verdict is exact rather than conservative, because no block straddles a
 * view boundary.
 */
bool emp_vmdesc_view_is_covered(struct emp_vmdesc *desc, unsigned long start,
			unsigned long end)
{
	struct emp_vmdesc_view *v;
	bool covered = false;

	spin_lock(&desc->view_lock);
	for (v = &desc->views; v; v = v->next) {
		if (unlikely(v->count == 0)) /* only for initial state */
			continue;
		if (v->start >= end)
			break;	/* sorted by start: nothing later intersects */
		if (v->end > start) {
			covered = true;
			break;
		}
	}
	spin_unlock(&desc->view_lock);
	return covered;
}

/**
 * emp_vmdesc_view_exit - release the view metadata of a dying vmdesc
 * @param desc vmdesc
 *
 * The last close empties every interval, so this normally frees nothing.
 */
void emp_vmdesc_view_exit(struct emp_vmdesc *desc)
{
	struct emp_vmdesc_view *v, *next;

	debug_assert(desc->views.count == 0);
	v = desc->views.next;
	desc->views.next = NULL;
	while (v) {
		next = v->next;
		emp_kfree(v);
		v = next;
	}
}
#endif /* CONFIG_EMP_USER */

struct emp_vmdesc *alloc_vmdesc(struct emp_vmdesc *prev)
{
	struct emp_vmdesc *desc;
	desc = emp_kzalloc(sizeof(struct emp_vmdesc), GFP_KERNEL);
	if (unlikely(desc == NULL))
		return NULL;
	if (prev)
		memcpy(desc, prev, sizeof(struct emp_vmdesc));
#ifdef CONFIG_EMP_USER
	atomic_set(&desc->refcount, 1);
	init_waitqueue_head(&desc->closing_wq);
	emp_vmdesc_view_init(desc);
#endif
	return desc;
}

int gpas_open(struct emp_vmr *vmr)
{
	int ret;
	struct emp_mm *emm = vmr->emm;
	if (vmr->descs == NULL) {
		struct emp_vmdesc *descs;
		descs = alloc_vmdesc(NULL);
		if (descs == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		descs->subblock_order = bvma_subblock_order(emm);
		vmr->descs = descs;
	}

	ret = allocate_gpas(emm, vmr);
	if (ret) {
		emp_kfree(vmr->descs);
		vmr->descs = NULL;
		goto out;
	}

#ifdef CONFIG_EMP_USER
	/* The first vmr of a new backing namespace. allocate_gpas() has just
	 * set vm_base, so the view coordinates are known now. The embedded
	 * entry is free, so this cannot fail and cannot allocate. */
	emp_vmdesc_view_add(vmr->descs, vmr_view_start(vmr),
					vmr_view_end(vmr), NULL);
	debug_check_vmdesc_views(vmr->descs);
#endif

out:
	return ret;
}

/**
 * gpas_close - cloase and remove emp_gpa and remote_page
 * @param vmr vmr to be closed
 * @param do_unmap if true, we need to unmap page table. if false, kernel already did unmap.
 * @parem must_wait if true, wait until the closing gpas are finished.
 */
void COMPILER_DEBUG gpas_close(struct emp_vmr *vmr, bool do_unmap, bool must_wait)
{
#ifdef CONFIG_EMP_BLOCKDEV
	bool blockdev_used = vmr->emm->mrs.blockdev_used;
#endif
	if (atomic_fetch_inc(&vmr->gpas_closing) > 0) {
		/* other thread have started closing gpas. */
		if (must_wait)
			/* uninterruptibly, for the reason in
			 * close_and_free_gpas(): returning early here lets
			 * emp_vma_close() free this vmr while another thread
			 * is still inside close_and_free_gpas() using it */
			wait_event(vmr->gpas_close_wq, vmr->descs == NULL);
		else
			/* I will not wait. Restore the value. */
			atomic_dec(&vmr->gpas_closing);
		return;
	}

	if (!vmr->descs)
		return;

	if (close_and_free_gpas(vmr, do_unmap) == 0) {
		/* No other vmr use the vm_desc. Free it */
#ifdef CONFIG_EMP_USER
		emp_vmdesc_view_exit(vmr->descs);
#endif
		emp_kfree(vmr->descs);
	}

	vmr->descs = NULL;
	if (atomic_read(&vmr->gpas_closing) > 1)
		/* Other thread is waiting for me.
		 * Note that we do not decrement gpas_closing.
		 * It marks that the gpas are closed. */
		/* uninterruptible waiter; see close_and_free_gpas() */
		wake_up(&vmr->gpas_close_wq);

#ifdef CONFIG_EMP_BLOCKDEV
	/* NOTE: io_schedule() should be called after all locks have been released.
	 *       Also, we cannot ensure @vmr is alive. Use the backup value. */
	if (blockdev_used)
		io_schedule();
#endif
}

/**
 * gpa_init - initialize the guest physical address space
 * @param emm emm data structure
 * @param vmr vmr to be initialized
 * @param init_subblock_order the subblock order this GPA use
 * @param init_block_order the block order this GPA use
 */
int gpa_init(struct emp_mm *emm)
{
	int i;

	emm->ftm.per_vcpu_free_lpages_len = PER_VCPU_FREE_LPAGES_LEN(emm, 8);
	emm->vops.unmap_gpas = unmap_gpas;
	emm->vops.free_gpa = free_gpa;
	emm->vops.set_gpa_remote = set_gpa_remote;
	emm->vops.sync_hpt_map_in_block = sync_hpt_map_in_block;
	emm->vops.clear_gpa_prefetched_hpt = clear_gpa_prefetched_hpt;

#ifdef CONFIG_EMP_BLOCK
	printk("GPA block: %d pages subblock: %d pages\n", 
				(1 << emm->config.block_order),
				(1 << emm->config.subblock_order));
#endif

	for (i = 0; i <= BLOCK_MAX_ORDER; i++)
		set_gpadesc_alloc(emm, i, NULL);
	spin_lock_init(&emm->gpadesc_alloc.lock);

	return 0;
}

/**
 * gpa_exit - free the guest physical address space
 * @param emm emp_mm data structure
 */
void gpa_exit(struct emp_mm *emm)
{
	int i;
	struct kmem_cache *cachep;

	emm->ftm.per_vcpu_free_lpages_len = 0;
	emm->vops.unmap_gpas = NULL;
	emm->vops.free_gpa = NULL;
	emm->vops.set_gpa_remote = NULL;

	for (i = 0; i <= BLOCK_MAX_ORDER; i++) {
		gpadesc_alloc_lock(emm);
		cachep = __get_gpadesc_alloc(emm, i);
		__set_gpadesc_alloc(emm, i, NULL);
		gpadesc_alloc_unlock(emm);
		if (!cachep)
			continue;
		emp_kmem_cache_destroy(cachep);
	}
}
