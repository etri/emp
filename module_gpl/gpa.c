#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/kvm_host.h>
#include <linux/sort.h>
#include "config.h"
#include "emp_type.h"
#include "gpa.h"
#include "vm.h"
#include "mm.h"
#include "alloc.h"
#include "hva.h"
#include "glue.h"
#include "reclaim.h"
#include "block-flag.h"
#include "page_mgmt.h"
#include "kvm_mmu.h"
#include "block.h"
#include "debug.h"
#include "pcalloc.h"

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

		/* checks whether there was tlb invalidation by other threads
		 * while executing this function. */
		cmpxchg_kvm_tlbs_dirty(kvm, dirty_count, 0);

		return true;
	}

	make_all_cpus_req_no_IPI(kvm, KVM_REQ_TLB_FLUSH);
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

#define set_gpa_block_order(g, o) do {} while(0)

#define emp_ext_init_gpa(e, g, i) do {} while(0)

static inline void
__set_gpadesc(struct emp_mm *emm, unsigned long idx, struct emp_gpa *g,
					struct gpadesc_region *region)
{
	emp_init_gpa(emm, g);
	set_gpa_block_order(g, region->block_order);
#ifdef CONFIG_EMP_VM
	if (region->lowmem_block)
		set_gpa_flags_if_unset(g, GPA_LOWMEM_BLOCK_MASK);
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
	desc_order = region->block_order - bvma_subblock_order(emm);
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
	struct emp_mm *emm = vmr->emm;
	struct emp_vmdesc *desc = vmr->descs;
	struct gpadesc_region *regions = desc->regions;
	unsigned long gpa_len = desc->gpa_len;
	unsigned long boundary[GPADESC_MAX_REGION];
	int num_boundary, i, num_region;
	u8 b_order, sb_order;
	unsigned long block_aligned_start, block_aligned_end;
#ifdef CONFIG_EMP_VM
	u8 low_order;
	unsigned long low_memory_end;
#endif

	b_order = bvma_block_order(emm);
	sb_order = bvma_subblock_order(emm);

#ifdef CONFIG_EMP_VM
	low_order = (u8) LOW_MEMORY_MAX_ORDER;

	/* Set low memory end */
	if (emm->ekvm.kvm && vmr->id == 0) {
		/* TODO: how can we know GFN of vm_start?
		 * low memory region is the first 2MB of VM, and we need to
		 * restrict the maximum order of the region. Unfortunately, we
		 * only know HVA range here. Thus, we use a heuristic: if vmr_id
		 * is 0, it is the first memory region (numa node) and its GFN
		 * is started from 0.
		 * We need to revise this. For example, add an IOCTL and let
		 * EMP know the GFN of each memory region (numa node) before VM
		 * starts.
		 */
		low_memory_end = get_num_low_memory_pages(emm) >> sb_order;
	} else
		low_memory_end = 0;
#endif

	/* gather information */
	block_aligned_start = sb_at_head;
	block_aligned_end = sb_at_tail < gpa_len ? gpa_len - sb_at_tail : 0;

	/* add boundaries */
	num_boundary = 0;
#ifdef CONFIG_EMP_VM
	if (low_memory_end > 0)
		boundary[num_boundary++] = low_memory_end;
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

		curr->block_order = order;

#ifdef CONFIG_EMP_VM
		curr->lowmem_block = curr->end <= low_memory_end
						? true : false;
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
					emm->id, vmr->id, i,
					r->start, r->end,
					r->block_order,
#ifdef CONFIG_EMP_VM
					r->lowmem_block ? 1 : 0,
#else
					-1,
#endif
					-1
					);
	}
#endif
}

static void prepare_gpadesc_alloc(struct emp_mm *emm, struct emp_vmdesc *desc)
{
	int r;
	int order;
	struct kmem_cache *cachep;

	for (r = 0; r < desc->num_region; r++) {
		order = desc->regions[r].block_order - bvma_subblock_order(emm);
		gpadesc_alloc_lock(emm);
		cachep = __get_gpadesc_alloc(emm, order);
		gpadesc_alloc_unlock(emm);
		if (cachep)
			continue;
		/* With the align in 3rd parameter, the offset in the block can
		 * be calculated by the memory address.
		 */
		cachep = emp_kmem_cache_create("gpadesc_alloc",
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
	struct vm_area_struct *vma = vmr->host_vma;

	va_sb_order = bvma_va_subblock_order(emm);

	vm_start = VA_ROUND_DOWN_ORDER(vma->vm_start, va_sb_order);
	vm_end = VA_ROUND_UP_ORDER(vma->vm_end, va_sb_order);

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
	 * The partial subblocks exist when vma->vm_start/end != vm_start/end.
	 */
	if (sb_at_head == 0 && vma->vm_start != vm_start)
		sb_at_head = bvma_sib_size(emm);
	if (sb_at_tail == 0 && vma->vm_end != vm_end)
		sb_at_tail = bvma_sib_size(emm);

	gpa_len = (vm_end - vm_start) >> va_sb_order;

	dprintk("%s vma:0x%lx--0x%lx gpa_len: 0x%lx block_order: 0x%x sb_order: 0x%x va_sb_order: 0x%lx sb_at_head: 0x%lx sb_at_tail: 0x%lx\n",
			__func__,
			vma->vm_start, vma->vm_end,
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
void free_gpa(struct emp_mm *bvma, struct emp_gpa *gpa, struct vcpu_var *cpu)
{
	int gpa_order;
	struct page *gpa_page;

#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	if (gpa->local_page)
		debug_page_ref_mark(-100, gpa->local_page, 0);
#endif
	gpa_order = gpa_subblock_order(gpa);
	gpa_page = cleanup_gpa(bvma, gpa);
	if (gpa_page) {
		int i;
		for (i = 0; i < gpa_subblock_size(gpa); i++)
			clear_page_state(bvma, gpa_page + i);
		_emp_lock_page(gpa_page, gpa_order);
	}

	if (!gpa_page || !is_head_page(gpa_page, bvma_subblock_order(bvma)))
		return;

	if (!push_local_free_page(bvma, gpa_page, cpu))
		_refill_global_free_page(bvma, gpa_page);
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
	for (i = 0; i < KVM_ADDRESS_SPACE_NUM; i++) {
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
	int sb_page_order = sb_head->sb_order + PAGE_SHIFT;

	sb_hva = head_hva + (sb_dist << sb_page_order);


	return sb_hva;
}

static inline int COMPILER_DEBUG
__get_sb_pages(struct vm_area_struct *vma, struct emp_gpa *head,
	       u64 head_hva, struct emp_gpa *sb_head)
{
	return (1 << sb_head->sb_order);
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
static bool COMPILER_DEBUG
__unmap_ptes(struct emp_vmr *vmr, struct emp_gpa *head, unsigned long head_hva,
					pmd_t *pmd, struct mmu_gather *tlb)
{
	struct emp_gpa *sb_head;
	struct emp_mm *emm = vmr->emm;
	struct mm_struct *mm = vmr->host_mm;
	unsigned int sb_order, sb_pages_len;
	bool mapped, accessed, dirty, tlb_flush_needed = false;
	int i;

	sb_order = bvma_subblock_order(emm);
	
	// block-grained dirty management. need to more finer?
	dirty = false;

	for_each_gpas(sb_head, head) {
		pte_t *ptep, pte;
		int pte_clear_count;
		struct page *sb_page, *page;
		unsigned long sb_hva, addr, pfn;

		sb_page = sb_head->local_page->page;
		if (page_mapcount(sb_page) == 0) {
			sb_head->local_page->vmr_id = vmr->id;
			debug_lru_set_vmr_id_mark(sb_head->local_page, vmr->id);
			emp_lp_remove_pmd(emm, sb_head->local_page, vmr->id);
			debug_lru_del_vmr_id_mark(sb_head->local_page, vmr->id);
			debug_assert(EMP_LP_PMDS_EMPTY(&sb_head->local_page->pmds));

			/* NOTE: RSS is not changed. @vmr is the only vmr and it is the owner. */
			continue;
		}

		mapped = false;
		accessed = false;

		____local_gpa_to_hva_and_len(vmr, sb_head, sb_hva, sb_pages_len);

		/* block is aligned */
		debug_assert(pmd == sb_head->local_page->pmds.pmd);
		if (unlikely(sb_head->local_page->vmr_id < 0)) {
			sb_head->local_page->vmr_id = vmr->id;
			debug_lru_set_vmr_id_mark(sb_head->local_page, vmr->id);
		}
		emp_lp_remove_pmd(emm, sb_head->local_page, vmr->id);
		debug_lru_del_vmr_id_mark(sb_head->local_page, vmr->id);
		if (sb_head->local_page->vmr_id != vmr->id)
			emp_update_rss_sub(vmr, sb_pages_len,
						DEBUG_RSS_SUB_UNMAP_PTES,
						sb_head, DEBUG_UPDATE_RSS_SUBBLOCK);

		ptep = pte_offset_map(pmd, sb_hva);
		pfn = pte_pfn(*ptep);

		pte_clear_count = 0;
		for (i = 0, addr = sb_hva, page = sb_page; i < sb_pages_len;
				i++, addr += PAGE_SIZE, ptep++, pfn++,
				page++) {
			pte = *ptep;
			/* kernel may be closing vma and concurrently unmap pte.
			 * Then, just skip the unmap. */
			if (unlikely(pte_pfn(pte) == 0UL))
				continue;
#ifdef CONFIG_EMP_DEBUG
			if (unlikely(pte_pfn(pte) != page_to_pfn(page))) {
				struct vm_area_struct *vma = vmr->host_vma;
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
					(unsigned long) sb_page, page_to_pfn(sb_page),
					sb_page->flags,
					i, (unsigned long) (page + i),
					page_to_pfn(page + i), (page + i)->flags,
					vma->vm_start, vma->vm_end,
					vma->vm_flags,
					vmr->descs->vm_base);
			}
#endif
			native_pte_clear(NULL, 0, ptep);
			flush_cache_page(vmr->host_vma, addr, pfn);
			/* THKIM: Temporally remove the code for TLB */
			tlb_remove_tlb_entry(tlb, ptep, addr);
			pte_clear_count++;

			if (!mapped && pte_accessible(mm, pte))
				mapped = true;
			if (!accessed && pte_young(pte))
				accessed = true;
			if (!dirty && pte_dirty(pte))
				dirty = true;
			kernel_page_remove_rmap(page, vmr->host_vma, false);
		}

		page_ref_sub(sb_page, pte_clear_count);
		debug_page_ref_mark(vmr->id, sb_head->local_page, -pte_clear_count);
		debug_check_lessthan(page_count(sb_page), 1);
		if (mapped) {
			if (accessed && 
				!PageReferenced(sb_head->local_page->page)) {
				SetPageReferenced(sb_head->local_page->page);
			}
			tlb_flush_needed = true;
		}
	}
	if (dirty) {
		set_gpa_flags_if_unset(head, GPA_DIRTY_MASK);
		SetPageDirty(head->local_page->page);
	}

	emp_update_rss_cached(vmr);

	return tlb_flush_needed;
}

/** unmap_ptes - Unmap page table entries from the host page table
 * @param bvma bvma data structure
 * @param gpas gpas to unmap
 * @param start start address
 * @param size total size
 * @param tlb TLB info
 */
static void unmap_ptes(struct emp_mm *bvma, struct emp_gpa *head,
		       unsigned long head_hva, unsigned long size)
{
	spinlock_t *ptl;
	bool tlb_flush_needed;
	unsigned int sb_order;

	struct emp_vmr *next_vmr;
	struct mmu_gather tlb;
	int next_vmr_id;
	struct mapped_pmd *p, *pp;

	sb_order = bvma_subblock_order(bvma);

	next_vmr = EMP_LP_PMDS_EMPTY(&head->local_page->pmds)? NULL:
			bvma->vmrs[head->local_page->pmds.vmr_id];
	next_vmr_id = -1;
	
	while (next_vmr) {
		kernel_tlb_gather_mmu(&tlb, next_vmr->host_mm,
				      head_hva, head_hva + size);

		if (emp_lp_lookup_pmd(head->local_page, next_vmr->id, &p, &pp)) {
			/* NOTE: we acquire and release page table lock at block
			 *       granularity to prevent deadlock with
			 *       __unmap_max_block().
			 */
			ptl = pte_lockptr(next_vmr->host_mm, p->pmd);
			spin_lock(ptl);
			tlb_flush_needed |= __unmap_ptes(next_vmr, head,
						head_hva, p->pmd, &tlb);
			spin_unlock(ptl);

			if (!EMP_LP_PMDS_EMPTY(&head->local_page->pmds))
				next_vmr_id = head->local_page->pmds.vmr_id;
		}

		kernel_tlb_finish_mmu(&tlb, head_hva, head_hva + size);

		if (next_vmr_id != -1) {
			next_vmr = bvma->vmrs[next_vmr_id];
			next_vmr_id = -1;
		} else {
			next_vmr = NULL;
		}
	}

	debug_unmap_ptes(bvma, head, size);
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
	struct emp_vmr *vmr = emm->vmrs[head->local_page->vmr_id];
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
		 * if (!hpt_mapped && head->local_page->vmr_id != vmr->id)
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

static inline void
__unmap_subblock_single_vmr(struct emp_vmr *vmr, struct emp_gpa *gpa,
			unsigned long hva, unsigned long page_len, pmd_t *pmd)
{
	pte_t *ptep;
	struct page *page;
	unsigned long pfn, i;

	debug_progress(gpa, vmr->id);
	debug_lru_progress_mark(gpa->local_page, vmr->id);

#ifdef CONFIG_EMP_VM
	debug_BUG_ON(is_gpa_flags_set(gpa, GPA_LOWMEM_BLOCK_MASK));
#endif

	ptep = pte_offset_map(pmd, hva);
	pfn = pte_pfn(*ptep);
	page = gpa->local_page->page;
	for (i = 0; i < page_len; i++, hva += PAGE_SIZE, ptep++, pfn++, page++) {
		/* kernel may be unmap this PTE due to the splitted vma
		 * Then, just skip the unmap. */
		if (unlikely(pte_pfn(*ptep) == 0UL))
			continue;
		debug_BUG_ON(pte_pfn(*ptep) != page_to_pfn(page));
		native_pte_clear(NULL, 0, ptep);
		flush_cache_page(vmr->host_vma, hva, pfn);
		tlb_remove_tlb_entry((&vmr->close_tlb), ptep, hva);
		kernel_page_remove_rmap(page, vmr->host_vma, false);
	}

	/* We do not use wrapper __emp_put_pages_map(),
	 * since __put_local_page_pmd() will sync the page ref for debug */
	____emp_put_pages_map(gpa, page_len);
}

static inline void
__unmap_max_block(struct emp_vmr *vmr, struct emp_gpa *max_head,
				unsigned long max_head_idx, int size)
{
	unsigned long i, head_idx, head_hva, sb_page_len;
	struct emp_gpa *head, *gpa;
	struct mapped_pmd *p, *pp;
	spinlock_t *ptl;

	for (i = 0, head = max_head; i < size;
			i += num_subblock_in_block(head),
			head += num_subblock_in_block(head)) {
		if (head->r_state != GPA_ACTIVE)
			continue;
		debug_assert(head->local_page);
		if (!emp_lp_lookup_pmd(head->local_page, vmr->id, &p, &pp))
			continue;

#ifdef CONFIG_EMP_VM
		/* DO NOT unmap the low memory region for VMs */
		if (unlikely(is_gpa_flags_set(head, GPA_LOWMEM_BLOCK_MASK)))
			continue;
#endif

		/* NOTE: we acquire and release page table lock at block
		 *       granularity to prevent deadlock with __unmap_ptes().
		 */
		ptl = pte_lockptr(vmr->host_mm, p->pmd);
		spin_lock(ptl);

		head_idx = max_head_idx + i;
		head_hva = GPN_OFFSET_TO_HVA(vmr, head_idx, head->sb_order);

		sb_page_len = gpa_subblock_size(head);
		for_each_gpas(gpa, head) {
#ifdef CONFIG_EMP_DEBUG
			struct mapped_pmd *sb_p, *sb_pp;
			if (!emp_lp_lookup_pmd(gpa->local_page, vmr->id, &sb_p, &sb_pp))
				BUG();
			/* block is aligned */
			debug_assert(p->pmd == sb_p->pmd);
#endif
			__unmap_subblock_single_vmr(vmr, gpa, head_hva, sb_page_len, p->pmd);
			head_hva += sb_page_len << PAGE_SHIFT;
		}
		emp_update_rss_sub(vmr, gpa_block_size(head),
					DEBUG_RSS_SUB_UNMAP_MAX_BLOCK,
					head, DEBUG_UPDATE_RSS_BLOCK);

		spin_unlock(ptl);
	}
}

static inline void
__put_local_page_pmd(struct emp_vmr *vmr, struct emp_gpa *gpa)
{
	int removed = 0;
	if (emp_lp_remove_pmd(vmr->emm, gpa->local_page, vmr->id)) {
		removed = 1;
		debug_lru_del_vmr_id_mark(gpa->local_page, vmr->id);
		debug_page_ref_unmap_end(gpa->local_page);
		debug_page_ref_mark(vmr->id, gpa->local_page, -1);
	}
	if (gpa->local_page->vmr_id == vmr->id) {
		gpa->local_page->vmr_id = gpa->local_page->pmds.vmr_id;
		debug_lru_set_vmr_id_mark(gpa->local_page,
						gpa->local_page->pmds.vmr_id);
		if (!removed)
			emp_update_rss_sub(vmr,
					__local_gpa_to_page_len(vmr, gpa),
					DEBUG_RSS_SUB_PUT_LOCAL_PAGE,
					gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
	}
}

/* remove vmr->id and pmd if inserted.
 * return head's refcnt.
 */
static inline int
__put_max_block(struct emp_mm *emm, struct vcpu_var *cpu,
		struct emp_vmr *vmr, int vm_refcnt,
		struct emp_vmr *next_vmr_shared,
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
				if (head->local_page->vmr_id >= 0)
					/* We know the other owner, keep it on list */
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

		/* Find the other owner. It is cheap than writeback.
		 * For MAP_SHARED, just pick any other vmr. All share gpa_dir.
		 * Note that desc is locked, and gpa_dir is stable.
		 * For MAP_PRIVATE, check parent and children. Note that gpas
		 * are locked. gpa_dir element is stable until unlock the gpa.
		 */


		debug_lru_progress_mark(head->local_page, head->r_state);
		debug_lru_progress_mark(head->local_page, head->flags);
		debug_lru_progress_mark(head->local_page, head->local_page->flags);
		/* For GPA_ACTIVE and GPA_INACTIVE, remove from the list */
		remove_gpa_from_lru(emm, head);

		debug_lru_progress_mark(head->local_page, head->flags);
		debug_lru_progress_mark(head->local_page, head->local_page->flags);
		debug_lru_progress_mark(head->local_page, refcnt);
		debug_lru_progress_mark(head->local_page, vm_refcnt);
	}

	debug_assert(refcnt >= 0);
	return refcnt + vm_refcnt;
}

static inline void
__wait_for_prefetch_max_block(struct emp_mm *emm, struct vcpu_var *cpu,
				struct emp_gpa *max_head, unsigned long size)
{
	unsigned long i;
	struct emp_gpa *gpa;
	for (i = 0, gpa = max_head; i < size;
			i += num_subblock_in_block(gpa),
			gpa += num_subblock_in_block(gpa)) {
		if (!is_gpa_flags_set(gpa, GPA_PREFETCHED_CPF_MASK))
			continue; // early check before function call
		wait_for_prefetch_subblocks(emm, cpu, &gpa, 1);
	}
}

void flush_gpa(struct emp_vmr *vmr, struct vcpu_var *cpu,
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
			/* TODO: it would be better that vcpu->wb_request_lock is locked during close_gpas  */
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

/* desc_refcnt: after decrement */
static unsigned long
free_gpa_dir_region(struct emp_vmr *vmr, struct vcpu_var *cpu,
		struct emp_gpa **gpa_dir, struct gpadesc_region *region,
		int vm_refcnt, bool do_unmap, struct emp_vmr *next_vmr_shared)
{
	struct emp_mm *emm = vmr->emm;
	unsigned long num_allocated = 0;
	struct emp_gpa *max_head;
	unsigned long i, step;
	int desc_order = region->block_order - bvma_subblock_order(emm);
	struct kmem_cache *cachep = get_gpadesc_alloc(emm, desc_order);
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	struct gpadesc_alloc_at_table *table;
	table = emp_vzalloc(sizeof(struct gpadesc_alloc_at_table)
					* (1 << GPADESC_ALLOC_AT_TABLE_SHIFT));
#endif
	step = 1UL << desc_order;
	// Don't use for_all_gpa_heads_range() here. desc_order may be different
	// from gpa_block_order(head) due to elastic block.
	for (i = region->start; i < region->end; i += step) {
		if (!gpa_dir[i])
			continue;
		max_head = gpa_dir[i];
		debug_free_gpa_dir_region(max_head, desc_order);
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
		gpadesc_alloc_at_insert(&max_head->alloc_at, table);
#endif
		__lock_max_block(max_head, step);
		if (do_unmap)
			__unmap_max_block(vmr, max_head, i, step);
#ifdef CONFIG_EMP_DEBUG_RSS
		else {
			/* if kernel unmaps page table entries,
			 * it also decreases RSS.
			 */
			int __i;
			struct emp_gpa *__head;
			struct mapped_pmd *p, *pp;
			for (__i = 0, __head = max_head; __i < step;
					__i += num_subblock_in_block(__head),
					__head += num_subblock_in_block(__head)) {
				if (__head->r_state != GPA_ACTIVE)
					continue;
				debug_assert(__head->local_page);
				if (!emp_lp_lookup_pmd(__head->local_page, vmr->id, &p, &pp))
					continue;
				emp_update_rss_sub_kernel(vmr,
					__local_block_to_page_len(vmr, __head),
					DEBUG_RSS_SUB_KERNEL_FREE_GPA_DIR,
					__head, DEBUG_UPDATE_RSS_BLOCK);
			}
		}
#endif
		if (__put_max_block(emm, cpu, vmr, vm_refcnt,
					next_vmr_shared, max_head, step,
					gpa_dir, i) > 0) {
			__unlock_max_block(max_head, step);
			continue;
		}

		__wait_for_prefetch_max_block(emm, cpu, max_head, step);
		flush_gpa(vmr, cpu, max_head, i, step);
		remote_page_release(emm, max_head, step);
		emp_kmem_cache_free(cachep, max_head);
		num_allocated += step;
	}
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	gpadesc_alloc_at_show(emm->id, vmr->id, region, table);
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
	struct emp_vmr *next_vmr_shared = NULL;

	if (unlikely(!desc->gpa_dir_alloc))
		return 0;
	
	cpu = emp_this_cpu_ptr(emm->pcpus);
#ifdef CONFIG_EMP_VM
	if (emm->ekvm.kvm) // this locking can be long, but VM is terminating.
		lock_kvm_mmu_lock(emm->ekvm.kvm);
#endif /* CONFIG_EMP_VM */

	spin_lock(&desc->lock); // Assure desc->refcnt is stable
	vm_refcnt = atomic_dec_return(&desc->refcount);

	if (do_unmap)
		dprintk("[DEBUG] %s: UNMAP emm: %d vmr: %d mm: %lx vma: %lx "
			"vm_base: 0x%lx size: 0x%lx vm_start: 0x%lx vm_end: 0x%lx\n",
			__func__, emm->id, vmr->id,
			(unsigned long) vmr->host_mm, (unsigned long) vmr->host_vma,
			vmr->descs->vm_base,
			vmr->descs->gpa_len << (vmr->descs->subblock_order + PAGE_SHIFT),
			vmr->host_vma->vm_start, vmr->host_vma->vm_end);

	if (do_unmap)
		kernel_tlb_gather_mmu(&vmr->close_tlb, vmr->host_mm,
				vmr->host_vma->vm_start, vmr->host_vma->vm_end);

	/* if the gpas pointer is still used, free the allocated memory and
	 * save NULL */
	for (r = 0; r < desc->num_region; r++)
		num_allocated += free_gpa_dir_region(vmr, cpu, desc->gpa_dir,
						&desc->regions[r], vm_refcnt,
						do_unmap, next_vmr_shared);

	if (do_unmap)
		kernel_tlb_finish_mmu(&vmr->close_tlb,
				vmr->host_vma->vm_start, vmr->host_vma->vm_end);
	spin_unlock(&desc->lock);

#ifdef CONFIG_EMP_VM
	if (emm->ekvm.kvm)
		unlock_kvm_mmu_lock(emm->ekvm.kvm);
#endif
	dprintk(KERN_INFO "%s: gpa descriptor allocation: %ld/%ld (%ld.%02ld%%)\n",
				__func__, num_allocated, desc->gpa_len,
				num_allocated * 100 / desc->gpa_len,
				num_allocated * 10000 / desc->gpa_len % 100);

	if (vm_refcnt > 0)
		return vm_refcnt;

	desc->gpa_dir = (struct emp_gpa **) NULL;
	emp_vfree(desc->gpa_dir_alloc);
	desc->gpa_dir_alloc = (void *) NULL;
	return 0;
}

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

	gpa->local_page = NULL;
	clear_gpa_flags_if_set(gpa, GPA_CLEANUP_MASK);
	if (local_page) {
		gpa_page = local_page->page;
		bvma->lops.free_local_page(bvma, local_page);
		gpa_page->private = 0;
		emp_clear_pg_mlocked(gpa_page, gpa_subblock_order(gpa));
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
	if (likely(g->local_page && g->local_page->vmr_id >= 0)) {
		struct emp_vmr *vmr = emm->vmrs[g->local_page->vmr_id];
		emp_update_rss_sub(vmr, __local_gpa_to_page_len(vmr, g),
					DEBUG_RSS_SUB_SET_REMOTE,
					g, DEBUG_UPDATE_RSS_SUBBLOCK);
	}

	emm->vops.free_gpa(emm, g, cpu);
	g->r_state = GPA_INIT;
	set_gpa_flags_if_unset(g, GPA_REMOTE_MASK);
}

struct emp_vmdesc *alloc_vmdesc(struct emp_vmdesc *prev)
{
	struct emp_vmdesc *desc;
	desc = emp_kzalloc(sizeof(struct emp_vmdesc), GFP_KERNEL);
	if (unlikely(desc == NULL))
		return NULL;
	if (prev)
		memcpy(desc, prev, sizeof(struct emp_vmdesc));
	spin_lock_init(&desc->lock);
	atomic_set(&desc->refcount, 1);
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

out:
	return ret;
}

/**
 * gpas_close - cloase and remove emp_gpa and remote_page
 * @param vmr vmr to be closed
 * @param do_unmap if true, we need to unmap page table. if false, kernel already did unmap.
 */
void COMPILER_DEBUG gpas_close(struct emp_vmr *vmr, bool do_unmap)
{
	if (spin_trylock(&vmr->gpas_close_lock) == false)
		return;

	if (!vmr->descs) {
		spin_unlock(&vmr->gpas_close_lock);
		return;
	}

	if (close_and_free_gpas(vmr, do_unmap) == 0)
		/* No other vmr use the vm_desc. Free it */
		emp_kfree(vmr->descs);

	vmr->descs = NULL;

	spin_unlock(&vmr->gpas_close_lock);
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

	emm->ftm.per_vcpu_free_lpages_len = PER_VCPU_FREE_LPAGES_LEN(emm, 2);
	emm->vops.unmap_gpas = unmap_gpas;
	emm->vops.free_gpa = free_gpa;
	emm->vops.set_gpa_remote = set_gpa_remote;


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
