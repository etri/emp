#include <linux/module.h>
#include <linux/errno.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/swap.h>
#include <linux/mmu_notifier.h>
#include <linux/list.h>
#include <linux/semaphore.h>
#include <asm/bitops.h>
#include "config.h"
#include "gpa.h"
#include "stat.h"
#include "vm.h"
#include "glue.h"
#include "block.h"
#include "els_block.h"
#include "reclaim.h"
#include "donor_mem_rw.h"
#include "alloc.h"
#include "hva.h"
#include "page_mgmt.h"
#include "udma.h"
#include "donor_mgmt.h"
#include "debug.h"
#include "kvm_emp.h"
#include "procfs.h"
#include "local_page.h"
#include "block-flag.h"
#include "debug.h"
#ifdef CONFIG_EMP_USER
#include "cow.h"
#include "pcalloc.h"
#endif
#include "ioctl.h"
#include "procfs.h"

#undef MEASURE_COMPONENTS

/* ----- global variables ----- */
size_t  initial_local_cache_pages; // counts in 4kb
#ifdef CONFIG_EMP_BLOCK
int     initial_block_order;
int     initial_subblock_order;
int     initial_critical_subblock_first;
int     initial_critical_page_first;
int     initial_mark_empty_page;
int     initial_mem_poll;
int     initial_enable_transition_csf;
#endif
#ifdef CONFIG_EMP_ELASTIC_BLOCK
int    initial_els_disabled;
#endif
#ifdef CONFIG_EMP_OPT
int     initial_eval_media;
int	initial_chained_ops;
int     initial_async_invlept;
int     initial_writeback_optimization_disable;
int     initial_eager_writeback;
#endif
int     initial_remote_reuse;
int     initial_remote_policy_subblock;
int     initial_remote_policy_block;

#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
#ifdef CONFIG_EMP_VM
atomic64_t num_emp_gpa_fault = {0};
#endif
atomic64_t num_emp_hva_fault = {0};
#endif

/* ----- local variables ----- */
static int                  emp_major = 0;
static struct class         *emp_class;
struct emp_mm               **emp_mm_arr;
unsigned long               emp_mm_arr_len;
spinlock_t                  emp_mm_arr_lock;
/* ----- local variables ----- */

#ifdef CONFIG_EMP_EXT
struct emp_ext emp_ext;
struct emp_ops emp_ops;
#define BVMA_SIZE (emp_ext.bvma_size ? emp_ext.bvma_size : sizeof(struct emp_mm))
#else
#define BVMA_SIZE (sizeof(struct emp_mm))
#endif

#ifdef CONFIG_EMP_OPT
/**
 * eager_wbr_ctor - Constructor for eager writeback list
 * @param opaque eager writeback entry
 */
static void eager_wbr_ctor(void *opaque)
{
	struct eager_wbr *w = (struct eager_wbr *)opaque;
	INIT_LIST_HEAD(&w->list);
}
#endif /* CONFIG_EMP_OPT */

#ifdef CONFIG_EMP_USER
static inline void finish_emp_vma_split(struct emp_vmr *vmr);
static void emp_vmr_discard(struct emp_vmr *vmr);
#endif

#ifdef CONFIG_EMP_EXT
/**
 * init_emp_ops - initialize emp_ops with default functions
 */
static void init_emp_ops(void) {
	emp_ops.handle_active_fault = handle_active_fault;
	emp_ops.handle_inactive_fault = handle_inactive_fault;
	emp_ops.handle_writeback_fault = handle_writeback_fault;
	emp_ops.handle_remote_fault = handle_remote_fault;
	emp_ops.reclaim_emp_pages = reclaim_emp_pages;
	emp_ops.reclaim_set = reclaim_set;
	emp_ops.update_lru_lists = update_lru_lists;
	emp_ops.add_gpas_to_inactive = add_gpas_to_inactive;
	emp_ops.emp_writeback_block = emp_writeback_block;
	emp_ops.remove_gpa_from_lru = remove_gpa_from_lru;
	emp_ops.adjust_local_cache_size = adjust_local_cache_size;
}

/**
 * unregister_emp_ext - Unregister EMP extension module
 * 
 * @retval 0: Success
 * @retval n: Error
 *
 */
int unregister_emp_ext() {
	if (!emp_ext.installed) {
		printk(KERN_ERR "[emp_ext] ERROR: no extension module installed\n");
		return -1;
	}

	init_emp_ops();
	
	/* installed = 0, name = NULL, bvma_size = 0, funcionts = NULL */
	memset(&emp_ext, 0, sizeof(struct emp_ext));
	return 0;
}
EXPORT_SYMBOL(unregister_emp_ext);

/**
 * register_emp_ext - Register EMP extension module
 * @param func advanced function pointers
 * @param ext  extension module information
 * 
 * @retval 0: Success
 * @retval n: Error
 *
 */
int register_emp_ext(struct emp_ext *ext) {
	if (emp_ext.installed) {
		printk(KERN_ERR "[emp_ext] ERROR: duplicated extension module\n");
		return -1;
	}
	
	emp_ext.name = ext->name;
	emp_ext.bvma_size = ext->bvma_size;
	emp_ext.memreg_size = ext->memreg_size;
#define APPLY_OPS(func) do { if (ext->ops.func) emp_ops.func = ext->ops.func; } while (0)
	APPLY_OPS(handle_active_fault);
	APPLY_OPS(handle_inactive_fault);
	APPLY_OPS(handle_writeback_fault);
	APPLY_OPS(handle_remote_fault);
	APPLY_OPS(reclaim_emp_pages);
	APPLY_OPS(reclaim_set);
	APPLY_OPS(update_lru_lists);
	APPLY_OPS(add_gpas_to_inactive);
	APPLY_OPS(emp_writeback_block);
	APPLY_OPS(remove_gpa_from_lru);
	APPLY_OPS(adjust_local_cache_size);
#undef APPLY
#define APPLY(func) do { if (ext->func) emp_ext.func = ext->func; } while (0)
	APPLY(emp_open);
	APPLY(emp_release);
	APPLY(emp_mmap);
	APPLY(emp_vma_open);
	APPLY(emp_vma_close);
	APPLY(emp_unlocked_ioctl);
	APPLY(emp_fsync);
	APPLY(create_mr);
	APPLY(disconnect_mr);
	APPLY(alloc_remote_page_notifier);
	APPLY(free_remote_page_notifier);
	APPLY(emp_set_block_dirty_notifier);
	APPLY(init_gpa);
#ifdef CONFIG_EMP_USER
	APPLY(dup_cow_gpa);
#endif
	APPLY(cleanup_gpa);
#ifdef CONFIG_EMP_USER
	APPLY(cleanup_cow_gpa);
#endif
	APPLY(waiting_writeback_notifier);
	APPLY(flush_gpa);

#ifdef CONFIG_EMP_VM
	APPLY(prepare_map_gpa);
	APPLY(early_handle_fault_gpa);
	APPLY(prepare_install_sptes);
#endif
	APPLY(prepare_map_hva);
	APPLY(early_handle_fault_hva);
	APPLY(prepare_install_hptes);
#ifdef CONFIG_EMP_VM
	APPLY(register_kvm);
#endif
#ifdef CONFIG_EMP_DEBUG
	APPLY(debug_emp_unlock_block);
#endif
#undef APPLY

	emp_ext.installed = 1;
	return 0;
}
EXPORT_SYMBOL(register_emp_ext);
#endif

/**
 * get_emp_mm_arr - Get the whole bvma array
 *
 * @return array of bvma
 */
struct emp_mm **get_emp_mm_arr(void)
{
	return emp_mm_arr;
}

#ifdef CONFIG_EMP_VM
static inline void set_kvm_emp_mm(struct kvm *kvm, void *emp_mm) {
	container_of(kvm, struct kvm_emp_container, kvm)->emp_mm = emp_mm;	
}
#endif

static int init_vcpu_var(struct vcpu_var *v, int id)
{
	int ret;
	memset(v, 0, sizeof(*v));
	(v)->id = id;
	ret = emp_pf_history_init(v);
	if (ret)
		return ret;

#ifdef CONFIG_EMP_OPT
	v->eager_wbr_cache = 
		emp_kmem_cache_create("eager_wbr",
				sizeof(struct eager_wbr), 0, 0,
				eager_wbr_ctor);
	if (v->eager_wbr_cache == NULL)
		return -ENOMEM;
#endif /* CONFIG_EMP_OPT */

	return 0;
}

#ifdef CONFIG_EMP_VM
static void __put_vcpus_var(struct emp_mm *bvma, int cpus_len);
/**
 * get_vcpus_var - Initialize vcpu_var data structure
 * @param bvma bvma data structure
 */
static int get_vcpus_var(struct emp_mm *bvma)
{
	struct vcpu_var *v;
	int i = 0, vcpus_size, vcpus_len = EMP_KVM_VCPU_LEN(bvma);
	int ret;

	if (vcpus_len == 0) {
		bvma->vcpus = NULL;
		return 0;
	}

	vcpus_size = sizeof(struct vcpu_var) * vcpus_len;
	bvma->vcpus = emp_kmalloc(vcpus_size, GFP_KERNEL);
	if (!bvma->vcpus) {
		ret = -ENOMEM;
		goto get_vcpus_var_fail;
	}

	/* per-cpu local free-page lists for KVM threads (contiguous array) */
	bvma->ftm.local_free_bufs = emp_kmalloc(sizeof(struct emp_list) * vcpus_len,
						GFP_KERNEL);
	if (!bvma->ftm.local_free_bufs) {
		ret = -ENOMEM;
		goto get_vcpus_var_fail;
	}
	for (i = 0; i < vcpus_len; i++)
		init_emp_list(&bvma->ftm.local_free_bufs[i]);

	for (i = 0; i < vcpus_len; i++) {
		v = &bvma->vcpus[i];
		ret = init_vcpu_var(v, i);
		if (ret)
			goto get_vcpus_var_fail;
	}
	return 0;

get_vcpus_var_fail:
	if (bvma->vcpus)
		__put_vcpus_var(bvma, i);
	printk(KERN_ERR "ERROR: failed to %s(). errno: %d\n",
				__func__, ret);
	return -ENOMEM;
}

static void __put_vcpus_var(struct emp_mm *bvma, int cpus_len)
{
	int cpu;
	struct vcpu_var *v;
	struct emp_list *free_page_list;

	if (bvma->vcpus == NULL)
		return;

	free_page_list = &bvma->ftm.free_page_list;
	emp_list_lock(free_page_list);
	for (cpu = 0; cpu < cpus_len; cpu++) {
		v = &bvma->vcpus[cpu];
		flush_local_free_pages(bvma, v);
	}
	emp_list_unlock(free_page_list);

#ifdef CONFIG_EMP_DEBUG_PF_HISTORY
	for (cpu = 0; cpu < cpus_len; cpu++) {
		v = &bvma->vcpus[cpu];
		if (v->pf_history)
			emp_kfree(v->pf_history);
	}
#endif
#ifdef CONFIG_EMP_OPT
	for (cpu = 0; cpu < cpus_len; cpu++) {
		v = &bvma->vcpus[cpu];
		if (v->eager_wbr_cache)
			emp_kmem_cache_destroy(v->eager_wbr_cache);
	}
#endif /* CONFIG_EMP_OPT */

	if (bvma->ftm.local_free_bufs) {
		emp_kfree(bvma->ftm.local_free_bufs);
		bvma->ftm.local_free_bufs = NULL;
	}
	emp_kfree(bvma->vcpus);
	bvma->vcpus = NULL;
}

/**
 * put_vcpus_var - Free vcpu_var data structure
 * @param emm emm data structure
 */
static void put_vcpus_var(struct emp_mm *emm)
{
	__put_vcpus_var(emm, EMP_KVM_VCPU_LEN(emm));
}
#endif /* CONFIG_EMP_VM */

static void __put_pcpus_var(struct emp_mm *emm, int max_cpu_id);

static int get_pcpus_var(struct emp_mm *emm)
{
	int ret, cpu = 0;
	struct vcpu_var *v;

	emm->pcpus = emp_alloc_percpu(struct vcpu_var);
	if (emm->pcpus == NULL) {
		ret = -ENOMEM;
		goto put_pcpus_var_fail;
	}

#ifdef CONFIG_EMP_DEBUG
	emm->debug_pcpus = emp_kzalloc(nr_cpu_ids * sizeof(struct vcpu_var *),
						GFP_KERNEL);
	if (emm->debug_pcpus == NULL) {
		emp_free_percpu(emm->pcpus);
		ret = -ENOMEM;
		goto put_pcpus_var_fail;
	}
#endif

	/* per-cpu local free-page lists for IO threads (NUMA-local, contiguous spine) */
	emm->ftm.host_free_bufs = emp_alloc_pcdata(struct emp_list);
	if (emm->ftm.host_free_bufs == NULL) {
		ret = -ENOMEM;
		goto put_pcpus_var_fail;
	}
	for_each_possible_cpu(cpu)
		init_emp_list(emp_pc_ptr(emm->ftm.host_free_bufs, cpu));

	for_each_possible_cpu(cpu) {
		v = per_cpu_ptr(emm->pcpus, cpu);
		ret = init_vcpu_var(v, VCPU_ID(emm, cpu));
		if (ret)
			goto put_pcpus_var_fail;
#ifdef CONFIG_EMP_DEBUG
		emm->debug_pcpus[cpu] = v;
#endif
	}
	return 0;

put_pcpus_var_fail:
	if (emm->pcpus == NULL)
		__put_pcpus_var(emm, cpu);
	printk(KERN_ERR "ERROR: failed to %s(). errno: %d\n",
				__func__, ret);
	return ret;
}

static void __put_pcpus_var(struct emp_mm *emm, int max_cpu_id)
{
	int cpu;
	struct vcpu_var *v;
	struct emp_list *free_page_list;

	if (emm->pcpus == NULL)
		return;

	free_page_list = &emm->ftm.free_page_list;
	emp_list_lock(free_page_list);
	for_each_possible_cpu(cpu) {
		if (unlikely(cpu >= max_cpu_id))
			break;
		v = per_cpu_ptr(emm->pcpus, cpu);
		flush_local_free_pages(emm, v);
	}
	emp_list_unlock(free_page_list);

#ifdef CONFIG_EMP_DEBUG_PF_HISTORY
	for_each_possible_cpu(cpu) {
		if (unlikely(cpu >= max_cpu_id))
			break;
		v = per_cpu_ptr(emm->pcpus, cpu);
		if (v->pf_history)
			emp_kfree(v->pf_history);
	}
#endif
#ifdef CONFIG_EMP_OPT
	for_each_possible_cpu(cpu) {
		if (unlikely(cpu >= max_cpu_id))
			break;
		v = per_cpu_ptr(emm->pcpus, cpu);
		if (v->eager_wbr_cache)
			emp_kmem_cache_destroy(v->eager_wbr_cache);
	}
#endif /* CONFIG_EMP_OPT */

	emp_free_pcdata(emm->ftm.host_free_bufs);
	emm->ftm.host_free_bufs = NULL;

	if (emm->pcpus) {
		emp_free_percpu(emm->pcpus);
		emm->pcpus = NULL;
	}
#ifdef CONFIG_EMP_DEBUG
	if (emm->debug_pcpus) {
		emp_kfree(emm->debug_pcpus);
		emm->debug_pcpus = NULL;
	}
#endif
}

static void put_pcpus_var(struct emp_mm *emm)
{
	__put_pcpus_var(emm, nr_cpu_ids);
}

#ifdef CONFIG_EMP_DEBUG
/* Give @vmr an index for diagnostics. Ids are reused so that they stay small;
 * nothing is looked up by one, so exhaustion is not an error. */
static void emp_vmr_debug_id_set(struct emp_mm *emm, struct emp_vmr *vmr)
{
	unsigned long p;

	spin_lock(&emm->debug_vmr_ids_lock);
	p = find_first_zero_bit(emm->debug_vmr_ids, EMP_DEBUG_VMR_IDS_MAX);
	if (likely(p < EMP_DEBUG_VMR_IDS_MAX)) {
		__set_bit(p, emm->debug_vmr_ids);
		vmr->debug_id = (int) p;
	} else
		vmr->debug_id = -1;
	spin_unlock(&emm->debug_vmr_ids_lock);
}

static void emp_vmr_debug_id_clear(struct emp_mm *emm, struct emp_vmr *vmr)
{
	if (emp_vmr_dbgid(vmr) < 0)
		return;
	spin_lock(&emm->debug_vmr_ids_lock);
	__clear_bit(emp_vmr_dbgid(vmr), emm->debug_vmr_ids);
	spin_unlock(&emm->debug_vmr_ids_lock);
	vmr->debug_id = -1;
}
#else
#define emp_vmr_debug_id_set(emm, vmr) do {} while (0)
#define emp_vmr_debug_id_clear(emm, vmr) do {} while (0)
#endif /* !CONFIG_EMP_DEBUG */

static void emp_vmr_register(struct emp_mm *emm, struct emp_vmr *vmr)
{
	write_lock(&emm->vmr_list_lock);
	list_add_tail(&vmr->vmr_list, &emm->vmrs_list);
	emm->num_vmrs++;
	write_unlock(&emm->vmr_list_lock);
}

static void emp_vmr_release(struct emp_vmr *vmr)
{
	struct emp_mm *emm = vmr->emm;

#ifdef CONFIG_EMP_USER
	debug_assert(vmr->mmu_notifier == NULL);
#endif

#ifdef CONFIG_EMP_VM
	if (emm->ekvm.lowmem_vmr == vmr)
		emm->ekvm.lowmem_vmr = NULL;
#endif

	emp_vmr_debug_id_clear(emm, vmr);

	write_lock(&emm->vmr_list_lock);
	list_del_init(&vmr->vmr_list);
	emm->num_vmrs--;
	write_unlock(&emm->vmr_list_lock);
}

static void emp_vma_close(struct vm_area_struct *vma)
{
	struct emp_vmr *vmr;

	vmr = __get_emp_vmr(vma);
	if (vmr == NULL)
		return;

       if (unlikely(vmr->host_vma != vma)) {
	       /* This must not be happened, but is easily avoided. */
               dprintk_ratelimited(KERN_WARNING
                       "%s: vma is not the owner of its vmr. emm: %d vmr: %d "
                       "vma: %016lx host_vma: %016lx virt: %016lx\n",
                       __func__, vmr->emm->id, emp_vmr_dbgid(vmr),
                       (unsigned long) vma, (unsigned long) vmr->host_vma,
                       vma->vm_start);
               vma->vm_private_data = NULL;
               return;
       }

#ifdef CONFIG_EMP_USER
	finish_emp_vma_split(vmr);
#endif

	dprintk_ratelimited("%s emm: %d num_vmr: %d vmr: %d vma:%016lx virt: %016lx "
				"vmr: %016lx desc: %016lx ref: %d\n",
			__func__, vmr->emm->id, vmr->emm->num_vmrs, emp_vmr_dbgid(vmr),
			(unsigned long) vma, vma->vm_start, (unsigned long) vmr,
			(unsigned long) vmr->descs,
			(int) (vmr->descs ? atomic_read(&vmr->descs->refcount) : -1));

	/* vmr->vmr_closing may be set by mmu notifier */
	if (vmr->vmr_closing == false) {
		vmr->vmr_closing = true;
		smp_mb();
		debug_show_gpa_state(vmr, __func__);
	}

#ifdef CONFIG_EMP_USER
#ifdef CONFIG_EMP_VM
	if (!vmr->emm->ekvm.kvm)
#endif /* CONFIG_EMP_VM */
		emp_put_mmu_notifier(vmr);
#endif /* CONFIG_EMP_USER */

#ifdef CONFIG_EMP_EXT
	if (emp_ext.emp_vma_close)
		emp_ext.emp_vma_close(vmr);
#endif

	/* gpas_close() may have been called by mmu notifier.
	 * In such case, vmr->descs == NULL and nothing happens by gpas_close().
	 */
	gpas_close(vmr, false, true); // also free the remote page

#ifdef CONFIG_EMP_DEBUG_RSS
	emp_update_rss_show(vmr);
#endif

	emp_vmr_release(vmr);

	vmr->host_vma = NULL;
	vmr->host_mm = NULL;
	emp_kfree(vmr);
}

#ifdef CONFIG_EMP_USER
/* Tile [gpa, gpa + num_gpa) with the largest blocks that fit, where @off is the
 * distance from the block head: a piece crossing the grid line of its own size
 * is not a head to emp_get_block_head(), and the split then skips it. */
static void
____split_set_max_block_order(struct emp_gpa *gpa, unsigned long off,
			      int num_gpa) {
	int sb_order = gpa_subblock_order(gpa);
	int block_order = gpa_block_order(gpa);
	int n = 0, i;

	while (n < num_gpa) {
		int size = num_gpa - n;
		int max_order = block_order;
		int max_desc = 1 << (max_order - sb_order);

		while (max_desc > size || (off & (max_desc - 1))) {
			max_order--;
			debug_assert(max_order >= sb_order);
			max_desc = 1 << (max_order - sb_order);
		}

		for (i = 0; i < max_desc; i++) {
			set_gpa_max_block_order(gpa, max_order);
			n++;
			gpa++;
		}
		off += max_desc;
	}
}

/* retval true: perfectly aligned with this head
 * retval false: need to reduce
 */
static bool
__split_set_max_block_order(struct emp_vmr *vmr, struct emp_gpa *head,
	unsigned long head_idx, unsigned long boundary_idx, unsigned long boundary_addr)
{
	struct emp_gpa *gpa, *boundary;
	unsigned long idx, end_idx = head_idx + num_subblock_in_block(head);
	int max_order, sb_order;
	unsigned long addr;
	unsigned int pg_len, pg_off;

	debug_assert(boundary_idx >= head_idx && boundary_idx < end_idx);

	sb_order = gpa_subblock_order(head);
	boundary = head + (boundary_idx - head_idx);
	____gpa_to_hva_len_off(vmr, boundary, boundary_idx, addr, pg_len, pg_off);

	if (head_idx == boundary_idx && boundary_addr == addr
			&& pg_len == gpa_subblock_size(boundary) && pg_off == 0) {
		/* perfectly aligned with this head */
		max_order = gpa_max_block_order(head);
		while (_emp_get_block_head(head, max_order) != head) {
			max_order--;
			debug_assert(max_order >= sb_order);
		}

		debug_assert(max_order >= gpa_block_order(head));

		if (max_order != gpa_max_block_order(head)) {
			for (gpa = head, idx = head_idx; idx < end_idx; gpa++, idx++)
				set_gpa_max_block_order(gpa, max_order);
		}
		return true;
	}

	/* before boundary */
	if (head_idx < boundary_idx)
		____split_set_max_block_order(head, 0, boundary_idx - head_idx);

	/* at boundary */
	if (boundary_addr != addr
			|| pg_len != gpa_subblock_size(boundary)
			|| pg_off != 0) {
		set_gpa_max_block_order(boundary, sb_order);
		set_gpa_flags_if_unset(boundary, GPA_PARTIAL_MAP_MASK);
		boundary_idx++;
		boundary++;
	}

	if (boundary_idx < end_idx)
		____split_set_max_block_order(boundary, boundary_idx - head_idx,
						end_idx - boundary_idx);

	return false;
}

/* @retval true: the block was handled here, including the transfer of its
 *               mapping records; the caller must not walk it again
 * @retval false: only the ceilings changed; the caller transfers the
 *                mapping records as it does for any other block
 */
static bool
__split_gpadesc(struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr,
		struct emp_gpa *head, unsigned long head_index,
		unsigned long boundary_index, unsigned long boundary_addr,
		bool new_is_front)
{
	struct emp_mm *emm = new_vmr->emm;
	struct emp_vmdesc *desc = prev_vmr->descs;
	struct emp_gpa *gpa;
	struct emp_gpa *boundary = desc->gpa_dir[boundary_index];
	int num_subblock = num_subblock_in_block(head);
	int sb_index;
	unsigned int flags;

	dprintk("%s new_vmr: %d prev_vmr: %d head_index: %lu boundary_index: %lu boundary_addr: %lu new_is_front: %d\n",
		__func__, emp_vmr_dbgid(new_vmr), emp_vmr_dbgid(prev_vmr),
		head_index, boundary_index, boundary_addr, new_is_front);

	if (__split_set_max_block_order(prev_vmr, head, head_index,
					boundary_index, boundary_addr))
		/* Aligned: the ceiling is capped and there is nothing to cut.
		 * The block lies wholly on one side. When that side is
		 * new_vmr's (new is the upper half and this is its first
		 * block), the caller must still run its transfer loop over it. */
		return new_is_front;

	clear_block_for_reduction(emm, prev_vmr, head, head_index);
	if (WB_BLOCK(head)) {
		/* writeback issued a block-level I/O.
		 * Before the block is splitted, wait for I/O completes,
		 * and clear work requests. */
		struct vcpu_var *cpu =
				emp_get_vcpu_from_id(emm, head->local_page->cpu);
		dprintk("%s wait for writeback completion. "
			"split head = %p split index = %ld\n",
			__func__, head, head_index);
		debug_progress(head->local_page->w, head);
		emm->sops.clear_writeback_block(emm, head, head->local_page->w,
			cpu, true, true);
	}

	for (sb_index = 1; sb_index < num_subblock; sb_index++)
		__emp_lock_block(head + sb_index);

	if (head->r_state == GPA_INIT) {
		flags = get_gpa_flags(head);
		for (sb_index = 0, gpa = head; sb_index < num_subblock; sb_index++, gpa++) {
			if (gpa_block_order(gpa) <= gpa_max_block_order(gpa))
				continue;
			__emp_els_stat_add(emm, gpa_block_order(gpa), block_count, -1);
			__emp_els_stat_add(emm, gpa_block_order(gpa), block_reduce, 1);

			set_gpa_block_order(gpa, gpa_max_block_order(gpa));
			if (emp_get_block_head(gpa) != gpa)
				continue;

			__emp_els_stat_add(emm, gpa_block_order(gpa), block_count, 1);
			if (gpa == head)
				continue;

			gpa->r_state = GPA_INIT;
			debug_assert(gpa->local_page == NULL);
			/* set_gpa_flags() does not clear any flags.
			 * GPA_PARTIAL_MAP_MASK is preserved. */
			set_gpa_flags(gpa, flags);
		}
	} else {
		struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);
		struct emp_gpa *new_heads[num_subblock];
		int n_new = 0;
		pmd_t *pmd;
		unsigned long boundary_gpa_addr = GPN_OFFSET_TO_HVA(prev_vmr,
					boundary_index, gpa_subblock_order(head));
		debug_BUG_ON(head->r_state == GPA_WB);
		debug_assert(head->r_state == GPA_ACTIVE
				|| head->r_state == GPA_INACTIVE);
#ifdef CONFIG_EMP_EXT
		emp_ops.remove_gpa_from_lru(emm, head);
#else
		remove_gpa_from_lru(emm, head);
#endif

#if defined(CONFIG_EMP_STAT) && defined(CONFIG_EMP_ELASTIC_BLOCK)
		__emp_els_stat_add(emm, gpa_block_order(head), block_count, -1);
		if (gpa_block_order(head) > gpa_max_block_order(head))
			__emp_els_stat_add(emm, gpa_block_order(head), block_reduce, 1);
#endif

		flags = get_gpa_flags(head);
		for (sb_index = 0, gpa = head; sb_index < num_subblock; sb_index++, gpa++) {
			debug_assert(gpa->local_page);
			debug_assert(emp_lp_owner(gpa->local_page)); /* ACTIVE or INACTIVE */
			if (gpa == boundary && is_gpa_flags_set(gpa, GPA_PARTIAL_MAP_MASK)
						&& boundary_addr != boundary_gpa_addr) {
				/* a new partial map gpa */
				pmd = emp_lp_lookup_pmd(gpa, prev_vmr);
				if (pmd) {
					emp_lp_insert_pmd(emm, gpa->local_page, new_vmr, pmd);
					debug_lru_add_vmr_id_mark(gpa->local_page, emp_vmr_dbgid(new_vmr));
					debug_page_ref_mark_map(emp_vmr_dbgid(new_vmr), gpa->local_page);
					/* RSS has been moved.
					 * However, host_mm of prev_vmr and new_vmr are identical.
					 * We don't need to take care of RSS actually. */
					emp_update_rss_sub_kernel(prev_vmr,
							__gpa_to_page_len(new_vmr, gpa, boundary_index),
							DEBUG_RSS_SUB_KERNEL_VMA_SPLIT,
							gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
					emp_update_rss_add_kernel(new_vmr,
							__gpa_to_page_len(new_vmr, gpa, boundary_index),
							DEBUG_RSS_ADD_KERNEL_VMA_SPLIT,
							gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
				}
			} else if (new_is_front ? gpa < boundary
						: gpa >= boundary) {
				bool owned = (emp_lp_owner(gpa->local_page)
							== prev_vmr);
				pmd = emp_lp_pop_pmd(emm, gpa->local_page, prev_vmr);
				if (pmd) {
					debug_lru_del_vmr_id_mark(gpa->local_page, emp_vmr_dbgid(prev_vmr));
					emp_lp_insert_pmd(emm, gpa->local_page, new_vmr, pmd);
					debug_lru_add_vmr_id_mark(gpa->local_page, emp_vmr_dbgid(new_vmr));
					debug_page_ref_mark_map(emp_vmr_dbgid(new_vmr), gpa->local_page);
				}
				if (owned && emp_lp_owner(gpa->local_page)
							!= new_vmr) {
					/* the representative follows the side
					 * that now views this subblock; both
					 * halves share one mm */
					if (emp_lp_count_pmd(gpa->local_page))
						emp_lp_promote_owner(
							gpa->local_page,
							new_vmr);
					else
						emp_lp_set_owner(
							gpa->local_page,
							new_vmr);
					debug_lru_set_vmr_id_mark(gpa->local_page, emp_vmr_dbgid(new_vmr));
				}
#ifdef CONFIG_EMP_DEBUG_RSS
				if (pmd || emp_lp_owner(gpa->local_page) == new_vmr) {
					/* RSS has been moved.
					 * However, host_mm of prev_vmr and new_vmr are identical.
					 * We don't need to take care of RSS actually. */
					emp_update_rss_sub_kernel(prev_vmr,
							__gpa_to_page_len(new_vmr, gpa, head_index + sb_index),
							DEBUG_RSS_SUB_KERNEL_VMA_SPLIT,
							gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
					emp_update_rss_add_kernel(new_vmr,
							__gpa_to_page_len(new_vmr, gpa, head_index + sb_index),
							DEBUG_RSS_ADD_KERNEL_VMA_SPLIT,
							gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
				}
#endif
			}

			if (gpa_block_order(gpa) > gpa_max_block_order(gpa))
				set_gpa_block_order(gpa, gpa_max_block_order(gpa));

			if (emp_get_block_head(gpa) != gpa)
				continue;

			__emp_els_stat_add(emm, gpa_block_order(gpa), block_count, 1);
			new_heads[n_new++] = gpa;

			if (gpa != head) {
				gpa->r_state = head->r_state;
				debug_assert(gpa->local_page != NULL);
				/* set_gpa_flags() does not clear any flags.
				 * GPA_PARTIAL_MAP_MASK is preserved. */
				set_gpa_flags(gpa, flags);
			}
		}

		if (head->r_state == GPA_ACTIVE) {
#ifdef CONFIG_EMP_EXT
			emp_ops.update_lru_lists(emm, cpu, new_heads, n_new, 0);
#else
			update_lru_lists(emm, cpu, new_heads, n_new, 0);
#endif
		} else {
#ifdef CONFIG_EMP_EXT
			emp_ops.add_gpas_to_inactive(emm, cpu, new_heads, n_new);
#else
			add_gpas_to_inactive(emm, cpu, new_heads, n_new);
#endif
		}
	}

#ifdef CONFIG_EMP_DEBUG
	/* every piece the stride walk lands on must also be a head by the
	 * pointer derivation, and lie on the grid the tiling assumed */
	for (gpa = head; gpa < head + num_subblock;
			gpa += num_subblock_in_block(gpa)) {
		debug_assert(emp_get_block_head(gpa) == gpa);
		debug_assert((gpa - head) % num_subblock_in_block(gpa) == 0);
	}
#endif

	for (sb_index = num_subblock - 1; sb_index > 0; sb_index--) {
#ifdef CONFIG_EMP_DEBUG
		/* for debug, call emp_unlock_block() for new block heads */
		struct emp_gpa *g = head + sb_index;
		if (emp_get_block_head(g) == g)
			emp_unlock_block(g);
		else
			__emp_unlock_block(g);
#else
		__emp_unlock_block(head + sb_index);
#endif
	}

	return true;
}

static void __split_vmdesc(struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr)
{
	struct emp_mm *emm = new_vmr->emm;
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	struct mm_struct *new_mm = new_vmr->host_mm;
#endif
	unsigned long head_idx, next_head_idx;
	unsigned long idx, idx_end;
	struct emp_gpa *gpa, *head;
	bool owned;
	unsigned long index_start, index_end;
	unsigned long prev_index_start, prev_index_end;
	unsigned long vpn, vpn_base, vpn_start, vpn_end;
	unsigned long prev_vpn_base, prev_vpn_start, prev_vpn_end;
	pmd_t *pmd;
	/* Index of the first subblock on the UPPER vma's side: the cut point */
	unsigned long boundary;

#ifdef CONFIG_EMP_DEBUG
	if (prev_vmr->vm_end == new_vmr->vm_start) {
		dprintk("%s prev_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx new_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx, block_aligned_start = %ld, gpa_len = %ld, num_region = %d\n",
				__func__, emp_vmr_dbgid(prev_vmr), prev_vmr->vm_start, prev_vmr->vm_end, emp_vmr_dbgid(new_vmr), new_vmr->vm_start, new_vmr->vm_end,
				prev_vmr->descs->block_aligned_start, prev_vmr->descs->gpa_len, prev_vmr->descs->num_region);
	} else if (new_vmr->vm_end == prev_vmr->vm_start) {
		dprintk("%s new_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx prev_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx, block_aligned_start = %ld, gpa_len = %ld, num_region = %d\n",
				__func__, emp_vmr_dbgid(new_vmr), new_vmr->vm_start, new_vmr->vm_end, emp_vmr_dbgid(prev_vmr), prev_vmr->vm_start, prev_vmr->vm_end,
				prev_vmr->descs->block_aligned_start, prev_vmr->descs->gpa_len, prev_vmr->descs->num_region);
	} else {
		printk(KERN_ERR "%s ERROR: prev_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx new_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx, block_aligned_start = %ld, gpa_len = %ld, num_region = %d\n",
				__func__, emp_vmr_dbgid(prev_vmr), prev_vmr->vm_start, prev_vmr->vm_end, emp_vmr_dbgid(new_vmr), new_vmr->vm_start, new_vmr->vm_end,
				prev_vmr->descs->block_aligned_start, prev_vmr->descs->gpa_len, prev_vmr->descs->num_region);

		BUG();
	}
#endif

	// new_vmr
	vpn_start = new_vmr->vm_start >> PAGE_SHIFT;
	vpn_end = new_vmr->vm_end >> PAGE_SHIFT;
	vpn_base = new_vmr->descs->vm_base >> PAGE_SHIFT;
	index_start = (vpn_start - vpn_base) >> bvma_subblock_order(emm);
	index_end = (vpn_end - vpn_base + bvma_subblock_size(emm) - 1)
						>> bvma_subblock_order(emm);

	// prev_vmr
	prev_vpn_start = prev_vmr->vm_start >> PAGE_SHIFT;
	prev_vpn_end = prev_vmr->vm_end >> PAGE_SHIFT;
	prev_vpn_base = prev_vmr->descs->vm_base >> PAGE_SHIFT;
	prev_index_start = (prev_vpn_start - prev_vpn_base) >> bvma_subblock_order(emm);
	prev_index_end = (prev_vpn_end - prev_vpn_base + bvma_subblock_size(emm) - 1)
						>> bvma_subblock_order(emm);

	if (prev_vmr->vm_end == new_vmr->vm_start) { /* [prev_vmr] + [new_wmr] */
		dprintk("%s prev_vmr(%d) prev_index_start = %ld prev_index_end = %ld new_vmr(%d) index_start = %ld index_end = %ld\n",
				__func__, emp_vmr_dbgid(prev_vmr), prev_index_start, prev_index_end, emp_vmr_dbgid(new_vmr), index_start, index_end);
		boundary = index_start;
	} else { /* [new_vmr] + [prev_wmr] */
		dprintk("%s new_vmr(%d) index_start = %ld index_end = %ld prev_vmr(%d) prev_index_start = %ld prev_index_end = %ld\n",
				__func__, emp_vmr_dbgid(new_vmr), index_start, index_end, emp_vmr_dbgid(prev_vmr), prev_index_start, prev_index_end);
		/* prev is the upper half here, and its first subblock is
		 * index_end - 1 when the split address falls inside a subblock
		 * and index_end when it is aligned. */
		boundary = prev_index_start;
	}
	/* assure that the boundary subblock exists */
	gpa = get_gpadesc(prev_vmr, boundary);

	/* When new_vmr is the LOWER half and the split address lands exactly on
	 * a subblock edge, @boundary == index_end and the block STARTING at the
	 * boundary would never be walked: nothing would lower its ceiling, and
	 * elastic block could merge it downward across the vma boundary. */
	if (boundary == index_end)
		index_end++;

	head_idx = index_start;
	while(head_idx < index_end) {
		head = get_next_exist_head_gpadesc(prev_vmr, &head_idx);
		if (head == NULL)
			break;

		head = emp_lock_block(prev_vmr, NULL, head_idx);
		next_head_idx = head_idx + num_subblock_in_block(head);

		if (unlikely(head_idx <= boundary && boundary < next_head_idx)) {
			bool new_is_front;
			unsigned long boundary_addr;

			if (prev_vmr->vm_end == new_vmr->vm_start) { /* [prev_vmr] + [new_wmr] */
				boundary_addr = new_vmr->vm_start;
				new_is_front = false;
			} else { /* [new_vmr] + [prev_wmr] */
				debug_assert(prev_vmr->vm_start == new_vmr->vm_end);
				boundary_addr = new_vmr->vm_end;
				new_is_front = true;
			}

			if (__split_gpadesc(new_vmr, prev_vmr,
						head, head_idx,
						boundary, boundary_addr,
						new_is_front))
				goto next_head;
		}

		/* Both halves view one namespace, so there is no directory to
		 * populate and no gpa reference to take: the slots the new vmr
		 * views are the slots which are already there. */

		if (INIT_BLOCK(head))
			goto next_head;

		vpn = (vpn_start & ~bvma_subblock_mask(emm))
			+ ((head_idx - index_start) << bvma_subblock_order(emm));

#ifdef CONFIG_EMP_DEBUG_PAGE_REF
		/* used for debug_check_page_map_status() */
		pmd = get_pmd(new_mm, vpn << PAGE_SHIFT);
#endif
		idx = head_idx;
		gpa = head;
		idx_end = next_head_idx;
		if (unlikely(idx < index_start)) {
			idx = index_start;
			gpa += idx - head_idx;
		}
		if (unlikely(idx_end > index_end))
			idx_end = index_end;

		while (idx < idx_end) {
			if (ACTIVE_BLOCK(head)) {
				if (!emp_lp_lookup_vmr(gpa, prev_vmr)) {
					debug_check_page_map_status(new_vmr, head,
								head_idx, pmd, false);
					goto next_gpa;
				}
                                
				debug_check_page_map_status(new_vmr, head,
								head_idx, pmd, true);
                                
				owned = (emp_lp_owner(gpa->local_page)
							== prev_vmr);
				pmd = emp_lp_pop_pmd(emm, gpa->local_page, prev_vmr);
				debug_lru_del_vmr_id_mark(gpa->local_page, emp_vmr_dbgid(prev_vmr));
                                
				emp_lp_insert_pmd(emm, gpa->local_page, new_vmr, pmd);
				debug_lru_add_vmr_id_mark(gpa->local_page, emp_vmr_dbgid(new_vmr));
				debug_page_ref_mark_map(emp_vmr_dbgid(new_vmr), gpa->local_page); /* mark the kernel's increment on page count */
				if (owned && emp_lp_owner(gpa->local_page)
							!= new_vmr) {
					emp_lp_promote_owner(gpa->local_page,
								new_vmr);
					debug_lru_set_vmr_id_mark(gpa->local_page, emp_vmr_dbgid(new_vmr));
				}
#ifdef CONFIG_EMP_DEBUG_RSS
				emp_update_rss_sub_kernel(prev_vmr,
						__gpa_to_page_len(new_vmr, gpa, idx),
						DEBUG_RSS_SUB_KERNEL_VMA_SPLIT,
						gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
				emp_update_rss_add_kernel(new_vmr,
						__gpa_to_page_len(new_vmr, gpa, idx),
						DEBUG_RSS_ADD_KERNEL_VMA_SPLIT,
						gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
#endif
			} else { // INACTIVE, WB
				if (emp_lp_owner(gpa->local_page) == prev_vmr) {
					emp_lp_set_owner(gpa->local_page, new_vmr);
					debug_lru_set_vmr_id_mark(gpa->local_page, emp_vmr_dbgid(new_vmr));
#ifdef CONFIG_EMP_DEBUG_RSS
					emp_update_rss_sub_kernel(prev_vmr,
							__gpa_to_page_len(new_vmr, gpa, idx),
							DEBUG_RSS_SUB_KERNEL_VMA_SPLIT,
							gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
					emp_update_rss_add_kernel(new_vmr,
							__gpa_to_page_len(new_vmr, gpa, idx),
							DEBUG_RSS_ADD_KERNEL_VMA_SPLIT,
							gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
#endif
				}
			}

next_gpa:
			/* NOTE: RSS is not updated since every pages belongs to same mm. */
			idx++;
			gpa++;
		}

next_head:
		emp_unlock_block(head);
		head_idx = next_head_idx;
	}
}

/*
 * A vma split makes a second view of one backing namespace, not a second
 * namespace. The two halves therefore share prev's vmdesc: its directory, its
 * regions[], and its vm_base origin all describe the backing, which the split
 * does not touch. Only the view metadata and the mapping ownership change, and
 * no gpa reference changes hands.
 */
static int split_vmdesc(struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr)
{
	struct emp_vmdesc *desc = prev_vmr->descs;
	dprintk("%s: new: %016lx prev: %016lx \n",
		__func__, (unsigned long) new_vmr, (unsigned long) prev_vmr);

	atomic_inc(&desc->refcount);
	new_vmr->descs = desc;

	__split_vmdesc(new_vmr, prev_vmr);

	return 0;
}
#endif /* CONFIG_EMP_USER */

static void __copy_vma_info(struct emp_vmr *vmr, struct vm_area_struct *vma)
{
	vmr->vm_start = vma->vm_start;
	vmr->vm_end = vma->vm_end;
	vmr->host_vma = vma;
	vmr->host_mm = vma->vm_mm;
}

static struct emp_vmr *create_vmr(struct emp_mm *emm, struct vm_area_struct *vma)
{
	const size_t emp_vmr_size = sizeof(struct emp_vmr);
	struct emp_vmr *new_vmr;

	new_vmr = emp_kzalloc(emp_vmr_size, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(new_vmr))
		return NULL;

	emp_vmr_register(emm, new_vmr);
	emp_vmr_debug_id_set(emm, new_vmr);

	new_vmr->emm = emm;
	if (vma)
		__copy_vma_info(new_vmr, vma);
	new_vmr->new_gpadesc = new_gpadesc;
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	new_vmr->set_gpadesc_alloc_at = set_gpadesc_alloc_at;
#endif

#ifdef CONFIG_EMP_USER
	/* kzalloc() already gives EMP_FORK_COW; be explicit since the default
	 * decides whether a fork child inherits the parent's backing. */
	new_vmr->fork_policy = EMP_FORK_COW;
#endif
	init_waitqueue_head(&new_vmr->gpas_close_wq);

	return new_vmr;
}

#ifdef CONFIG_EMP_USER
/**
 * fork_find_sibling_vmdesc - the child namespace a sibling child vma already made
 * @param prev_vmr the parent vmr this child vma was duplicated from
 * @param new_vma the child vma being opened
 *
 * @return the child vmdesc to share, or NULL to create a fresh one
 *
 * A private fork of a range which has been split must preserve the grouping:
 * several parent vmrs viewing one namespace have to become several child vmrs
 * viewing one child namespace, not one namespace each.
 *
 * The sibling is found rather than remembered, so there is no fork-local state
 * to keep alive and nothing to invalidate. dup_mmap() walks the parent's vmas in
 * ascending order and creates exactly one child per parent vma, and it holds
 * both mmap locks throughout -- this acquires neither. So if the parent vma
 * which ends where prev_vmr begins views prev_vmr's namespace, then the child
 * vma which ends where this one begins is that vma's child, and its namespace is
 * the one to share.
 *
 * Every pointer dereferenced here belongs to a vma that is currently live in a
 * locked mm. If the child vma tree cannot be queried yet on some kernel, this
 * returns NULL and the fork falls back to one child namespace per child vmr,
 * which is correct but keeps the halves apart.
 */
static struct emp_vmdesc *
fork_find_sibling_vmdesc(struct emp_vmr *prev_vmr, struct vm_area_struct *new_vma)
{
	struct vm_area_struct *vma;
	struct emp_vmr *vmr;

	if (unlikely(prev_vmr->vm_start == 0 || new_vma->vm_start == 0))
		return NULL;

	/* does the parent's preceding vma view the same namespace? */
	vma = find_vma(prev_vmr->host_mm, prev_vmr->vm_start - 1);
	if (vma == NULL || vma->vm_end != prev_vmr->vm_start
			|| vma->vm_ops != &emp_vma_ops)
		return NULL;
	vmr = __get_emp_vmr(vma);
	if (vmr == NULL || vmr->descs != prev_vmr->descs)
		return NULL;

	/* then its child is this vma's predecessor, and holds the child
	 * namespace this half belongs in */
	vma = find_vma(new_vma->vm_mm, new_vma->vm_start - 1);
	if (vma == NULL || vma->vm_end != new_vma->vm_start
			|| vma->vm_ops != &emp_vma_ops)
		return NULL;
	vmr = __get_emp_vmr(vma);
	if (vmr == NULL || vmr->descs == NULL)
		return NULL;

	debug_assert(vmr->descs->vm_base == prev_vmr->descs->vm_base);
	return vmr->descs;
}

static struct emp_vmr * COMPILER_DEBUG
__emp_vma_open(struct emp_vmr *prev_vmr, struct vm_area_struct *new_vma)
{
	struct emp_mm *emm = prev_vmr->emm;
	struct emp_vmr *new_vmr;
	bool vm_shared = new_vma->vm_flags & VM_SHARED ? true : false;
	bool vm_wipeonfork = prev_vmr->fork_policy == EMP_FORK_WIPE ? true : false; // new_vmr inherits prev_vmr->fork_policy
	bool new_vmdesc, dup_dir; // options of dup_vmdesc

	new_vmr = create_vmr(emm, new_vma);
	if (new_vmr == NULL)
		return NULL;

	if (!is_emm_with_kvm(emm)) {
		if (emp_get_mmu_notifier(new_vmr)) {
			emp_kfree(new_vmr);
			return NULL;
		}
	}

	new_vma->vm_private_data = (void *)new_vmr;

	/* A fork child inherits the parent's fork policy, so a grandchild of a
	 * MADV_EMP_WIPEONFORK range is wiped as well. */
	new_vmr->fork_policy = prev_vmr->fork_policy;

	if (vm_shared) {
		new_vmr->descs = prev_vmr->descs;
		atomic_inc(&prev_vmr->descs->refcount);
		/* One lifetime reference and one view interval unit per vmr.
		 * A shared fork reproduces the parent's interval exactly, so an
		 * identical entry is already there and nothing is allocated. */
		if (unlikely(emp_vmdesc_view_add(new_vmr->descs,
						vmr_view_start(new_vmr),
						vmr_view_end(new_vmr), true)))
			printk(KERN_ERR "%s: ERROR: no view entry to share. "
					"vmr: [0x%lx, 0x%lx) vm_base: 0x%lx\n",
					__func__, new_vmr->vm_start,
					new_vmr->vm_end,
					new_vmr->descs->vm_base);
		new_vmdesc = false;
		dup_dir = false;
	} else if (vm_wipeonfork) {
		new_vmdesc = true;
		dup_dir = false;
	} else { // map private
		/* If this range was split, its halves view one namespace in the
		 * parent and must view one child namespace too. The half opened
		 * before this one has already made it. */
		struct emp_vmdesc *sibling;

		sibling = fork_find_sibling_vmdesc(prev_vmr, new_vma);
		if (sibling) {
			/* a second view of the child namespace: a distinct
			 * interval, so it needs an entry of its own */
			new_vmr->descs = sibling;
			atomic_inc(&sibling->refcount);
			if (unlikely(emp_vmdesc_view_add(sibling,
						vmr_view_start(new_vmr),
						vmr_view_end(new_vmr), false)))
				printk(KERN_ERR "%s: ERROR: no view entry for "
						"the forked split half. "
						"vmr: [0x%lx, 0x%lx)\n",
						__func__, new_vmr->vm_start,
						new_vmr->vm_end);
			new_vmdesc = false;
		} else
			new_vmdesc = true;
		dup_dir = true;
	}

	dprintk_ratelimited("%s emm: %d num_vmr: %d vmr: %d "
			"vma:%016lx virt: %016lx flags: %lx "
			"vmr: %016lx shared: %d wipeonfork: %d\n",
		__func__, emm->id, emm->num_vmrs, emp_vmr_dbgid(new_vmr),
		(unsigned long) new_vma, new_vma->vm_start, new_vma->vm_flags,
		(unsigned long) new_vmr, vm_shared, vm_wipeonfork);

	if (dup_vmdesc(new_vmr, prev_vmr, new_vmdesc, dup_dir)) {
		if (vm_shared || !new_vmdesc) {
			/* give back what the shared or sibling branch took */
			emp_vmdesc_view_del(new_vmr->descs,
						vmr_view_start(new_vmr),
						vmr_view_end(new_vmr));
			atomic_dec(&new_vmr->descs->refcount);
		}
		new_vma->vm_private_data = NULL;
		emp_vmr_release(new_vmr);
		new_vmr->host_vma = NULL;
		new_vmr->host_mm = NULL;
		emp_kfree(new_vmr);
		return NULL;
	}

	debug_check_vmdesc_views(new_vmr->descs);

	return new_vmr;
}

static inline void finish_emp_vma_split(struct emp_vmr *vmr)
 {
	if (vmr->split_new_vmr) {
		struct emp_vmr *split_vmr = vmr->split_new_vmr;
		debug_assert(split_vmr->split_prev_vmr == vmr);
		debug_assert(split_vmr->split_new_vmr == NULL);
		split_vmr->split_prev_vmr = NULL;
		vmr->split_new_vmr = NULL;
#ifdef CONFIG_EMP_DEBUG
		split_vmr->split_addr = 0;
#endif
		/* __split_vma() can fail after ->may_split() installed the link
		 * and before ->open() runs: vm_area_dup(), vma_iter_prealloc(),
		 * vma_dup_policy() and anon_vma_clone() all bail out in that
		 * window, and the cleanup that follows is
		 * mpol_put/vma_iter_free/vm_area_free -- no vm_op at all, so EMP
		 * is never told. __emp_vma_split() is what gives the new side a
		 * host_vma (and its vmdesc reference, via split_vmdesc()), so a
		 * linked vmr without one never reached ->open() and nothing will
		 * ever free it. */
		if (unlikely(split_vmr->host_vma == NULL))
			emp_vmr_discard(split_vmr);
	}

	if (vmr->split_prev_vmr) {
		struct emp_vmr *split_vmr = vmr->split_prev_vmr;
		debug_assert(split_vmr->split_new_vmr == vmr);
		debug_assert(split_vmr->split_prev_vmr == NULL);
		split_vmr->split_new_vmr = NULL;
		vmr->split_prev_vmr = NULL;
#ifdef CONFIG_EMP_DEBUG
		split_vmr->split_addr = 0;
#endif
	}
}

/* Release a vmr that never became a vma's. It holds only what create_vmr()
 * gave it: the emm vmr-list slot and a debug id. */
static void emp_vmr_discard(struct emp_vmr *vmr)
{
	debug_assert(vmr->host_vma == NULL);
	debug_assert(vmr->descs == NULL);
	debug_assert(vmr->split_prev_vmr == NULL);
	debug_assert(vmr->split_new_vmr == NULL);
	emp_vmr_release(vmr);
	emp_kfree(vmr);
}

static void __emp_vma_split(struct emp_vmr *prev_vmr, struct emp_vmr *new_vmr,
				struct vm_area_struct *new_vma)
{
	/* the interval prev_vmr views before the split. vm_base does not move,
	 * so this stays valid in the shared namespace afterwards. */
	unsigned long prev_start = vmr_view_start(prev_vmr);
	unsigned long prev_end = vmr_view_end(prev_vmr);
	unsigned long mid;

	debug_assert(prev_vmr->split_addr == new_vmr->split_addr);
	debug_assert(new_vmr->split_addr == new_vma->vm_start
			|| new_vmr->split_addr == new_vma->vm_end);

	__copy_vma_info(new_vmr, new_vma);

	dprintk("%s (BEFORE) prev: vm_start=%016lx vm_end=%016lx "
		"new_vmr: vm_start=%016lx vm_end=%016lx\n",
			__func__, prev_vmr->vm_start, prev_vmr->vm_end,
			new_vmr->vm_start, new_vmr->vm_end);

	if (prev_vmr->vm_start == new_vmr->vm_start) {
		debug_assert(new_vmr->vm_end == new_vmr->split_addr);
		prev_vmr->vm_start = new_vmr->vm_end;
		mid = new_vmr->vm_end;
	} else {
		debug_assert(new_vmr->vm_start == new_vmr->split_addr);
		debug_assert(prev_vmr->vm_end == new_vmr->vm_end);
		prev_vmr->vm_end = new_vmr->vm_start;
		mid = new_vmr->vm_start;
	}
	/* the boundary, in the coordinates of the namespace both halves view.
	 * new_vmr has no vmdesc of its own yet, so use prev's origin. */
	mid = (mid - prev_vmr->descs->vm_base) >> PAGE_SHIFT;

	dprintk("%s  (AFTER) prev: vm_start=%016lx vm_end=%016lx "
		"new_vmr: vm_start=%016lx, vm_end=%016lx\n",
			__func__, prev_vmr->vm_start, prev_vmr->vm_end,
			new_vmr->vm_start, new_vmr->vm_end);

	split_vmdesc(new_vmr, prev_vmr);

	/* One view interval unit becomes two, on the same namespace. The union
	 * of coverage is unchanged, so a close in another mm may run against
	 * either side of this without losing a slot either half still views.
	 * Backing geometry is not touched here: the gpa refcounts, the
	 * directory, and regions[] are properties of the namespace. */
	if (unlikely(emp_vmdesc_view_split(prev_vmr->descs, prev_start, prev_end, mid)))
		printk(KERN_ERR "%s: ERROR: cannot partition the view of "
				"[0x%lx, 0x%lx) at 0x%lx\n", __func__,
				prev_start, prev_end, mid);
	debug_check_vmdesc_views(prev_vmr->descs);

	/* both halves of the split keep the fork policy of the original range */
	new_vmr->fork_policy = prev_vmr->fork_policy;

	new_vmr->vmr_closing = false;
	new_vma->vm_private_data = (void *)new_vmr;

	finish_emp_vma_split(prev_vmr);
}

// consider only the vma_open right after vma_ops->split
// argument vma is newly created vma and vma stored in vmr is prevous one
static void COMPILER_DEBUG emp_vma_open(struct vm_area_struct *new_vma)
{
	struct emp_vmr *prev_vmr = (struct emp_vmr *)new_vma->vm_private_data;
	struct emp_vmr *new_vmr;
	/* A split stays inside the same mm; dup_mmap() always builds the vma in
	 * the child's mm. Check it rather than inferring "not a split == fork",
	 * so that an open we do not model (mremap, or a future kernel change)
	 * becomes visible instead of being silently treated as a fork. */
	bool same_mm = new_vma->vm_mm == prev_vmr->host_mm;

	new_vma->vm_private_data = NULL;
	new_vmr = prev_vmr->split_new_vmr;
	if (new_vmr) {
		// emp_vma_split allocated new_vmr pointed by new_vmr of prev_vmr
		debug_BUG_ON(!same_mm);
		__emp_vma_split(prev_vmr, new_vmr, new_vma);
	} else {
		// open of dup_mmap reaches here
		if (unlikely(same_mm))
			printk_ratelimited(KERN_WARNING
				"%s: vma open of the same mm without a pending "
				"split. emm: %d vmr: %d vma: %016lx\n",
				__func__, prev_vmr->emm->id, emp_vmr_dbgid(prev_vmr),
				(unsigned long) new_vma);
		new_vmr = __emp_vma_open(prev_vmr, new_vma);
		if (unlikely(new_vmr == NULL))
			/* Failed to __emp_vma_open(). Stop here. */
			return;
	}

	vm_flags_set(new_vma, VM_MIXEDMAP | VM_NOHUGEPAGE | VM_DONTEXPAND);

#ifdef CONFIG_EMP_USER
	/* EMP does its own lazy fork: keep the kernel from copying this vma's
	 * PTEs so a fork child starts with empty page tables and EMP's metadata
	 * COW is the only mechanism inheriting the parent's contents.
	 *
	 * Set here rather than in emp_vma_open(): dup_mmap() decides the child's
	 * anon_vma *before* it calls ->open, so turning the flag on later would
	 * suppress the copy but leave the child with an anon_vma a real
	 * VM_WIPEONFORK vma would not have.
	 *
	 * The flag is never cleared. The user's MADV_WIPEONFORK/MADV_KEEPONFORK
	 * intent lives in emp_vmr.fork_policy instead; see enum emp_fork_policy.
	 */
	if (!(new_vma->vm_flags & VM_SHARED))
		vm_flags_set(new_vma, VM_WIPEONFORK);
#endif /* CONFIG_EMP_USER */

#ifdef CONFIG_EMP_EXT
	if (emp_ext.emp_vma_open)
		emp_ext.emp_vma_open(new_vmr);
#endif /* CONFIG_EMP_EXT */

	/* Actually, after duplication, gpa states of two vmrs are identical.
	 * Thus, we only print out new's. */
	debug_show_gpa_state(new_vmr, "emp_vma_open(new)");

	emp_update_rss_show(new_vmr);
}

static int emp_vma_split(struct vm_area_struct *vma, unsigned long addr)
{
	struct emp_vmr *prev_vmr = vma->vm_private_data;
	struct emp_vmr *new_vmr;

	if (addr & bvma_va_subblock_mask(prev_vmr->emm)) {
		printk(KERN_NOTICE "%s vma split. vma %llx addr %lx\n",
				__func__, (u64)vma, addr);
	}
	dprintk("%s vma split. vma %llx addr %lx\n",
				__func__, (u64)vma, addr);

	new_vmr = create_vmr(prev_vmr->emm, NULL);
	if (new_vmr == NULL) {
		printk("%s cannot allocate memory for new_vmr.\n", __func__);
		return -ENOMEM;
	}

	/* NOTE: we don't call emp_get_mmu_notifier() here.
	 * Since this is split of previous emp_vmr, which already registered
	 * emp's mmu notifier to its mm.
	 * In addition, new_vmr is created but has no host_vma. This causes
	 * errors in emp_get_mmu_notifier().
	 */

	/* clean up the previous split */
	finish_emp_vma_split(prev_vmr);
	/* start the new split */
	prev_vmr->split_new_vmr = new_vmr;
	new_vmr->split_prev_vmr = prev_vmr;
#ifdef CONFIG_EMP_DEBUG
	prev_vmr->split_addr = addr;
	new_vmr->split_addr = addr;
#endif

	return 0;
}
#endif /* CONFIG_EMP_USER */

struct vm_operations_struct emp_vma_ops = {
#ifdef CONFIG_EMP_USER
	.open = emp_vma_open,
#endif
	.close = emp_vma_close,
#ifdef CONFIG_EMP_USER
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0))
	.split = emp_vma_split,
#else
	.may_split = emp_vma_split,
#endif
#endif
	.fault = emp_page_fault_hva,
};

/**
 * emp_mmap - mmap for EMP
 * @param filp emp device file pointer
 * @param vma virtual memory area info of EMP
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * This function is called in QEMU's function when mapping VM's memory
 * on the host memory. \n
 * It maps memory and initializes EMP's components
 */
static int COMPILER_DEBUG
emp_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret = 0, i;
	struct emp_mm *bvma;
	size_t mem_size;
	struct emp_vmr *vmr;
#ifdef CONFIG_EMP_VM
	struct mm_struct *mm = vma->vm_mm;
	unsigned long new_start = vma->vm_start;
	unsigned long new_end = vma->vm_end;
#endif

	if (!filp->private_data)
		return -ENODEV;
	bvma = (struct emp_mm *)filp->private_data;

#ifdef CONFIG_EMP_VM
	if (bvma->ekvm.kvm &&
			((vma->vm_start & ~HPAGE_MASK) || 
			 (vma->vm_end & ~HPAGE_MASK))) {
		unsigned long old_start, old_end;
		size_t length;
		off_t shift;
		struct vm_area_struct *new_vma;

		old_start = vma->vm_start;
		old_end = vma->vm_end;
		length = old_end - old_start;

		shift = vma->vm_start & ~HPAGE_MASK;
		new_start = vma->vm_start - shift;
		new_end = vma->vm_end - shift;

		new_vma = find_vma(mm, new_start);
		if (new_end <= new_vma->vm_start)
			goto vm_start_aligned;

		shift = HPAGE_SIZE - shift;
		new_start = vma->vm_start + shift;
		new_end = vma->vm_end + shift;

		new_vma = find_vma(mm, new_start);
		if (new_end <= new_vma->vm_start)
			goto vm_start_aligned;

		return -EFAULT;
	}

vm_start_aligned:
	if (vma->vm_start != new_start) {
		vma->vm_start = new_start;
		vma->vm_end = new_end;
		dprintk("%s aligned vm_start: 0x%lx vm_end: 0x%lx\n",
				__func__, new_start, new_end);
	}
#endif /* CONFIG_EMP_VM */

#ifndef CONFIG_EMP_USER
	if (unlikely(!is_emm_with_kvm(bvma))) {
		printk(KERN_ERR "[ERROR] the current version of EMP does not support user-level processes.\n");
		return -ENODEV;
	}
#endif

	printk(KERN_NOTICE "%s emm: %d num_vmr: %d mm: %016lx vma:%016lx "
				"vm_start: %016lx vm_end: %016lx flags: %lx\n",
			__func__, bvma->id, bvma->num_vmrs,
			(unsigned long) vma->vm_mm, (unsigned long) vma,
			vma->vm_start, vma->vm_end, vma->vm_flags);

	might_sleep();

	// check whether requested memory size can be served by donor
	mem_size = 0;
	if (!bvma->config.remote_reuse)
		mem_size += atomic_read(&bvma->ftm.local_cache_pages);
	spin_lock(&bvma->mrs.memregs_lock);
	for (i = 0; i < MAX_MRID; i++) {
		if (bvma->mrs.memregs[i] == NULL)
			continue;
		mem_size += bvma->mrs.memregs[i]->size;
	}
	spin_unlock(&bvma->mrs.memregs_lock);
	mem_size <<= PAGE_SHIFT;
	if (mem_size < (vma->vm_end - vma->vm_start))
		return -ENOMEM;

	vmr = create_vmr(bvma, vma);
	if (vmr == NULL)
		return -ENOMEM;

#ifdef CONFIG_EMP_VM
	/* the first vmr a KVM-backed emp_mm maps holds the guest's low memory */
	if (bvma->ekvm.kvm && bvma->ekvm.lowmem_vmr == NULL)
		bvma->ekvm.lowmem_vmr = vmr;
#endif

#ifdef CONFIG_EMP_USER
	if (!is_emm_with_kvm(bvma)) {
		long ret = emp_get_mmu_notifier(vmr);
		if (unlikely(ret))
			return (int) ret;
	}
#endif /* CONFIG_EMP_USER */

	if (gpas_open(vmr))
		goto mmap_fail;

#ifdef CONFIG_EMP_EXT
	if (emp_ext.emp_mmap) {
		ret = emp_ext.emp_mmap(vmr);
		if (ret) goto mmap_fail;
	}
#endif
	bvma->ftm.local_cache_pages_headroom = LOCAL_CACHE_BUFFER_SIZE(bvma);

	// prevent numa from relocating the related pages
	vm_flags_set(vma, VM_MIXEDMAP | VM_NOHUGEPAGE | VM_DONTEXPAND);

	/* private data first: from the moment vm_ops is set, this vma is an EMP
	 * vma and its private data will be read as one */
	vma->vm_private_data = (void *)vmr;
	vma->vm_ops = &emp_vma_ops;

#ifdef CONFIG_EMP_VM
	if (bvma->ekvm.kvm && (bvma->ekvm.apic_base_hva == 0UL))
		bvma->ekvm.apic_base_hva = __gfn_to_hva(bvma, vmr,
				APIC_DEFAULT_PHYS_BASE >> PAGE_SHIFT);
#endif

	dprintk("%s (2) mm:%016lx vma:%016lx vm_flags: 0x%016lx "
		"vm_start: 0x%lx vm_end: 0x%lx\n", __func__,
		(unsigned long) vma->vm_mm, (unsigned long) vma,
		vma->vm_flags, vma->vm_start, vma->vm_end);
	return ret;

mmap_fail:
	gpas_close(vmr, false, false);
	emp_vmr_release(vmr);
	return -ENOMEM;
}

#ifdef CONFIG_EMP_VM
/**
 * register_mem_slot - register memory slot using vma info
 * @param bvma bvma data structure
 * @param start start address of vm's memory
 * @param size vm's total memory size
 *
 * Register memslot using vm's memory info
 */
void register_mem_slot(struct emp_mm *bvma, unsigned long start, unsigned long size)
{
	struct kvm_memory_slot *slot;

	debug_BUG_ON(bvma->ekvm.memslot_len > 2);

	slot = gfn_to_memslot(bvma->ekvm.kvm, gpa_to_gfn(start));
	if (slot) {
		int i, memslot;
		unsigned long base = 0;
		FOR_EACH_MEMSLOT(bvma, i)
			base += bvma->ekvm.memslot[i].size;
		memslot = bvma->ekvm.memslot_len++;
		bvma->ekvm.memslot[memslot].base = base;
		bvma->ekvm.memslot[memslot].gpa = start;
		bvma->ekvm.memslot[memslot].size = size;
		bvma->ekvm.memslot[memslot].hva =
			__gfn_to_hva_memslot(slot, gpa_to_gfn(start));

		dprintk("mem region registered: "
			"gpa: %lx hva: %lx size: %lx base:%lx\n",
			start, bvma->ekvm.memslot[memslot].hva, size, base);
	}
}

/**
 * register_kvm - Register KVM functions for mapping EMP functions to use
 * @param bvma bvma data structure
 * @param kvm_fd kvm file descriptor
 * @param kvm_max_vcpus maximum vcpus
 *
 * @retval true: Success
 * @retval false: Error
 *
 * Replace functionalities of KVM to EMP's functions
 */
int COMPILER_DEBUG
register_kvm(struct emp_mm *bvma, int kvm_fd, int kvm_max_vcpus)
{
	struct file *kvm_filp;

	kvm_filp = fget(kvm_fd);
	if (!kvm_filp)
		return -ENOENT;

#ifndef CONFIG_EMP_DONT_RESTRICT_LOW_MEMORY_REGION
	if (bvma->config.subblock_order > LOW_MEMORY_MAX_ORDER) {
		printk(KERN_ERR "ERROR: [emp] the maximum subblock order for VM"
				" should be less than or equal to"
				" LOW_MEMORY_MAX_ORDER(%d), but the subblock"
				" order is %d\n",
			LOW_MEMORY_MAX_ORDER,
			bvma->config.subblock_order);
		return -EINVAL;
	}
#endif

	bvma->ekvm.kvm = (struct kvm *)kvm_filp->private_data;
	set_kvm_emp_mm(bvma->ekvm.kvm, (void *)bvma);

	if (bvma->ekvm.kvm_vcpus_len == 0) {
		int prev_vcpus_len = EMP_KVM_VCPU_LEN(bvma);

		bvma->ekvm.kvm_vcpus_len = 
			atomic_read(&bvma->ekvm.kvm->online_vcpus);
		if (bvma->ekvm.kvm_vcpus_len == 0)
			bvma->ekvm.kvm_vcpus_len = kvm_max_vcpus;
		bvma->ekvm.emp_vcpus_len = bvma->ekvm.kvm_vcpus_len
						+ VCPU_START_ID;

		if (prev_vcpus_len < EMP_KVM_VCPU_LEN(bvma)) {
			__put_vcpus_var(bvma, prev_vcpus_len);
			reclaim_exit(bvma);

			if (get_vcpus_var(bvma))
				goto reg_kvm_get_vcpus_err;
			if (reclaim_init(bvma))
				goto reg_kvm_reclaim_init_err;
#ifdef CONFIG_EMP_EXT
			if (emp_ext.register_kvm &&
				!emp_ext.register_kvm(bvma, prev_vcpus_len))
				goto reg_kvm_reg_kvm_ext_err;
#endif
		}
	}

	printk(KERN_INFO "kvm is registered. the number of vcpus: %d\n",
			bvma->ekvm.kvm_vcpus_len);

	fput(kvm_filp);
	kvm_get_kvm(bvma->ekvm.kvm);

	return true;

#ifdef CONFIG_EMP_EXT
reg_kvm_reg_kvm_ext_err:
	reclaim_exit(bvma);
#endif
reg_kvm_reclaim_init_err:
	put_vcpus_var(bvma);
reg_kvm_get_vcpus_err:
	return false;
}
#endif /* CONFIG_EMP_VM */

/**
 * register_bvma - Initialize the virtual memory information for each VM used in EMP
 * @param bvma bvma data structure
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int register_bvma(struct emp_mm *bvma)
{
	int i, emp_index, ret;

	ret = -EBUSY;
	emp_index = -1;
	spin_lock(&emp_mm_arr_lock);
	for (i = 0; i < EMP_MM_MAX; i++) {
		if (emp_mm_arr[i] == NULL) {
			emp_index = i;
			break;
		}
	}
	if (emp_index != -1) {
		bvma->id = emp_index;
		emp_mm_arr_len++;
		emp_mm_arr[emp_index] = bvma;
		ret = 0;
	}
	spin_unlock(&emp_mm_arr_lock);

	return ret;
}

/**
 * unregister_bvma - Destroy the virtual memory information for each VM used in EMP
 * @param bvma bvma data structure
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int unregister_bvma(struct emp_mm *bvma)
{
	if (emp_mm_arr[bvma->id] == NULL ||
			emp_mm_arr[bvma->id] != bvma)
		return -EINVAL;

	spin_lock(&emp_mm_arr_lock);
	emp_mm_arr[bvma->id] = NULL;
	emp_mm_arr_len--;

	debug_unregister_bvma(bvma);
	spin_unlock(&emp_mm_arr_lock);
	return 0;
}

static DEFINE_MUTEX(emp_open_mutex);

static void cleanup_emm(struct emp_mm *emm)
{
	emm->close = 1;

	synchronize_rcu();
	synchronize_srcu(&emm->srcu);
	cleanup_srcu_struct(&emm->srcu);
}

static struct emp_mm *create_emm(void) 
{
	struct emp_mm *bvma;
	
	bvma = (struct emp_mm *)emp_kzalloc(BVMA_SIZE, GFP_KERNEL);
	if (!bvma) {
		printk(KERN_ERR "ERROR: failed to allocate bvma structure (size: %ld)\n",
							BVMA_SIZE);
		return NULL;
	}
	bvma->possible_cpus = num_possible_cpus();

	if (donor_mgmt_init(bvma))
		goto err;

	init_srcu_struct(&bvma->srcu);
#ifdef CONFIG_EMP_DEBUG
	spin_lock_init(&bvma->debug_vmr_ids_lock);
	bitmap_zero(bvma->debug_vmr_ids, EMP_DEBUG_VMR_IDS_MAX);
#endif
	INIT_LIST_HEAD(&bvma->vmrs_list);
	rwlock_init(&bvma->vmr_list_lock);
	spin_lock_init(&bvma->mrs.memregs_lock);
	init_waitqueue_head(&bvma->mrs.mrs_ctrl_wq);

	atomic_set(&bvma->refcount, 1);
	atomic_set(&bvma->ftm.alloc_pages_len, 0);

	init_emp_list(&bvma->ftm.free_page_list);
	init_waitqueue_head(&bvma->ftm.free_pages_wq);

#ifdef CONFIG_EMP_BLOCK
	bvma->config.subblock_order = initial_subblock_order;
	bvma->config.block_order = max(initial_block_order,
			initial_subblock_order);
	bvma->config.critical_subblock_first = initial_critical_subblock_first;
	bvma->config.critical_page_first = initial_critical_page_first;
	bvma->config.enable_transition_csf = initial_enable_transition_csf;
	bvma->config.mark_empty_page = initial_mark_empty_page;
	bvma->config.mem_poll = initial_mem_poll;
#endif
#ifdef CONFIG_EMP_STAT
	bvma->config.reset_after_read = 0;
#endif
#ifdef CONFIG_EMP_OPT
	bvma->config.next_pt_premapping = 0; /* TODO: test this feature */
	bvma->config.eval_media = initial_eval_media;
	bvma->config.chained_ops = initial_chained_ops;
	bvma->config.async_invlept = initial_async_invlept;
	bvma->config.writeback_optimization_disable = initial_writeback_optimization_disable;
	bvma->config.eager_writeback = initial_eager_writeback;
#endif
	bvma->config.remote_reuse = initial_remote_reuse;
	bvma->config.remote_policy_subblock = initial_remote_policy_subblock;
	bvma->config.remote_policy_block = initial_remote_policy_block;
	atomic_set(&bvma->ftm.local_cache_pages, initial_local_cache_pages);
	bvma->config.minimum_pages = atomic_read(&bvma->ftm.local_cache_pages) >> 3;

	emp_stat_init(bvma);

	bvma->ftm.local_cache_pages_headroom = 0;

	return bvma;
err:
	emp_kfree(bvma);
	return NULL;
}

static void free_bvma(struct emp_mm *bvma) 
{
	if (!bvma) return;
	if (bvma->mrs.memregs)
		emp_kfree(bvma->mrs.memregs);
	emp_kfree(bvma);
}

/*
 * In 6.5+ kernels, folio_mark_dirty() dispatches through
 * mapping->a_ops->dirty_folio with no fallback. EMP sets
 * page->mapping = vma->vm_file->f_mapping for VM_SHARED regions
 * (see emp_set_page_mapping_and_index in hva.h) to support shared
 * futexes. The default chardev aops on some kernels has dirty_folio
 * NULL, so munmap() oopses. Override the chardev's mapping aops
 * once with a private table that uses noop_dirty_folio. */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
static const struct address_space_operations emp_aops = {
	.dirty_folio = noop_dirty_folio,
};
#endif

/**
 * @param filp EMP device file pointer
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int emp_open(struct inode *inode, struct file *filp)
{
	int ret = 0;
	struct emp_mm *bvma;

	dprintk("%s[%d] f_flags: 0x%x\n", __func__, __LINE__, filp->f_flags);

	try_module_get(THIS_MODULE);
	mutex_lock(&emp_open_mutex);

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
	if (inode->i_mapping && inode->i_mapping->a_ops != &emp_aops)
		inode->i_mapping->a_ops = &emp_aops;
#endif

	if ((filp->f_flags & O_ACCMODE) == O_RDONLY) {
		printk(KERN_ERR "%s: failed to open: inappropriate flags\n",
				EMP_DEVICE_NAME);
		ret = -EINVAL;
		goto open_err;
	}

	if (emp_mm_arr_len >= EMP_MM_MAX) {
		printk(KERN_ERR "%s: failed to open: no vma slot\n",
				EMP_DEVICE_NAME);
		ret = -EBUSY;
		goto open_err;
	}

	bvma = create_emm();
	if (!bvma) {
		printk(KERN_ERR "%s: failed to open: lack of memory\n",
				EMP_DEVICE_NAME);
		ret = -ENOMEM;
		goto open_new_bvma_err;
	}

#ifdef CONFIG_EMP_USER
	cow_init(bvma);
#endif
	donor_mem_rw_init(bvma);
	dma_open(bvma);
	filp->private_data = bvma;

	ret = register_bvma(bvma);
	if (ret != 0) {
		printk(KERN_ERR "%s: failed to register emp vma\n",
				EMP_DEVICE_NAME);
		goto open_register_err;
	}
	printk(KERN_NOTICE "%s (emm registered) emm: %d num_emp_mm: %ld\n",
			__func__, bvma->id, emp_mm_arr_len);

	if (emp_procfs_add(bvma, bvma->id)) {
		ret = -ENOENT;
		goto open_procfs_err;
	}

#ifdef CONFIG_EMP_ELASTIC_BLOCK
	els_init(bvma, initial_els_disabled);
#endif

	if (remote_page_init(bvma))
		goto open_rp_init_err;
#ifdef CONFIG_EMP_VM
	if (get_vcpus_var(bvma))
		goto open_alloc_vcpu_err;
#endif
	if (get_pcpus_var(bvma))
		goto open_get_pcpus_var;
	if (reclaim_init(bvma))
		goto open_reclaim_init_err;
	if (local_page_init(bvma))
		goto open_lp_init_err;
	gpa_init(bvma);

#ifdef CONFIG_EMP_EXT
	if (emp_ext.emp_open) {
		ret = emp_ext.emp_open(bvma);
		if (ret) 
			goto open_procfs_err;
	}
#endif
	bvma->pid = current->pid;

	mutex_unlock(&emp_open_mutex);
	printk(KERN_NOTICE "%s (exit) emm: %d pid: %d current: %d num_vmrs: %d num_emp_mm: %ld\n",
			__func__, bvma->id, bvma->pid, current->pid, bvma->num_vmrs, emp_mm_arr_len);
	return ret;

open_procfs_err:
	local_page_exit(bvma);
open_lp_init_err:
	reclaim_exit(bvma);
open_reclaim_init_err:
	put_pcpus_var(bvma);
open_get_pcpus_var:
#ifdef CONFIG_EMP_VM
	put_vcpus_var(bvma);
open_alloc_vcpu_err:
#endif /* CONFIG_EMP_VM */
	remote_page_exit(bvma);
open_rp_init_err:
	unregister_bvma(bvma);
	dma_release(bvma);
open_register_err:
	free_bvma(bvma);
open_new_bvma_err:
	filp->private_data = NULL;
open_err:
	mutex_unlock(&emp_open_mutex);
	module_put(THIS_MODULE);
	return ret;
}

#ifdef CONFIG_KVM_ALLOC_PROFILE
void kvm_alloc_show_stat(void);
#endif

/**
 * emp_release - Release bvma for VM's address space & unregister EMP data structures
 * @param inode inode data structure
 * @param filp EMP device file pointer
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int emp_release(struct inode *inode, struct file *filp)
{
	struct emp_mm *bvma;

	dprintk("%s: filp->private_data=0x%lx\n", __func__,
			(unsigned long)filp->private_data);

	if (!filp->private_data)
		return -ENODEV;
	bvma = (struct emp_mm *)filp->private_data;
	printk(KERN_NOTICE "%s (begin) emm: %d pid: %d current: %d num_vmrs: %d num_emp_mm: %ld\n",
			__func__, bvma->id, bvma->pid, current->pid, bvma->num_vmrs, emp_mm_arr_len);

	WARN_ON(atomic_dec_and_test(&bvma->refcount) != true);

	mutex_lock(&emp_open_mutex);

	cleanup_emm(bvma);
#ifdef CONFIG_EMP_EXT
	if (emp_ext.emp_release)
		emp_ext.emp_release(bvma);
#endif
	reclaim_exit(bvma);
	gpa_exit(bvma);

#ifdef CONFIG_EMP_VM
	if (bvma->vcpus)
		put_vcpus_var(bvma);
#endif
	if (bvma->pcpus)
		put_pcpus_var(bvma);

	remote_page_exit(bvma);
	local_page_exit(bvma);
	alloc_exit(bvma);
#ifdef CONFIG_EMP_USER
	cow_exit(bvma);
#endif

	unregister_bvma(bvma);

	donor_mgmt_exit(bvma);
	dma_release(bvma);

#ifdef CONFIG_EMP_VM
	if (bvma->ekvm.kvm) {
		set_kvm_emp_mm(bvma->ekvm.kvm, NULL);

		kvm_put_kvm(bvma->ekvm.kvm);
	}
#endif

	printk(KERN_NOTICE "%s (exit) emm: %d pid: %d current: %d num_vmrs: %d num_emp_mm: %ld\n",
			__func__, bvma->id, bvma->pid, current->pid, bvma->num_vmrs, emp_mm_arr_len);

	emp_procfs_del(bvma);
	emp_kfree(bvma);

#ifdef CONFIG_KVM_ALLOC_PROFILE
	kvm_alloc_show_stat();
#endif

	mutex_unlock(&emp_open_mutex);
	module_put(THIS_MODULE);


	return 0;
}

static int emp_fsync(struct file *filp, loff_t s, loff_t e, int datasync)
{
	struct emp_mm *bvma;

	printk(KERN_DEBUG "%s called s: %llx e: %llx datasync: %d",
			__func__, s, e, datasync);

	if (!filp->private_data)
		return -EINVAL;

	bvma = (struct emp_mm *)filp->private_data;
	if (!bvma)
		return -EINVAL;

	emp_stat_inc(bvma, fsync_count);
#ifdef CONFIG_EMP_EXT
	if (emp_ext.emp_fsync)
		return emp_ext.emp_fsync(bvma, s, e, datasync);
	else
		return 0;
#else
	return 0;
#endif
}

static const struct file_operations emp_fops = {
	.open		= emp_open,
	.release	= emp_release,
	.mmap		= emp_mmap,
	.unlocked_ioctl	= emp_unlocked_ioctl,
	.fsync		= emp_fsync,
};

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 3)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0))
static int emp_uevent(const struct device *dev, struct kobj_uevent_env *env)
#else
static int emp_uevent(struct device *dev, struct kobj_uevent_env *env)
#endif
{
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}

static int check_assumption(void)
{
	if (sizeof(struct emp_gpa) != 64 && sizeof(struct emp_gpa) != 32) {
#ifdef CONFIG_EMP_DEBUG
		printk(KERN_WARNING "warning on data structure: emp_gpa: %ld != 64 or 32\n",
				sizeof(struct emp_gpa));
#else
		printk(KERN_ERR "error on data structure: emp_gpa: %ld != 64 or 32\n",
				sizeof(struct emp_gpa));
		return -EINVAL;
#endif
	} else
		dprintk(KERN_ERR "[EMP] emp_gpa descriptor size: %ld\n",
				sizeof(struct emp_gpa));
	return 0;
}

/**
 * emp_init - Initialize EMP module
 *
 * Initialize configurations and register character device
 */
static int __init emp_init(void)
{
	int ret = 0;
	struct device *err_dev;
#ifdef CONFIG_EMP_VM
	struct emp_mod emp_mod = {
		.page_fault = emp_page_fault_gpa,
#ifdef CONFIG_EMP_OPT
		.mark_free_pages = emp_mark_free_pages,
#else
		.mark_free_pages = NULL,
#endif
		.lock_range_pmd = emp_lock_range_pmd,
		.unlock_range_pmd = emp_unlock_range_pmd,
	};
#endif /* CONFIG_EMP_VM */

	if (check_assumption())
		return -EINVAL;

	if (kernel_symbol_init())
		return -ENOSYS;

	emp_debug_alloc_init();

#ifdef CONFIG_EMP_EXT
	init_emp_ops();
#endif

#ifdef CONFIG_EMP_VM
	register_emp_mod(&emp_mod);
#endif /* CONFIG_EMP_VM */

	emp_mm_arr = emp_kzalloc(EMP_MM_MAX * sizeof(struct emp_mm *), GFP_KERNEL);
	if (!emp_mm_arr) {
		printk(KERN_ERR "failed to allocate memory for vmas\n");
		return -ENOMEM;
	}
	emp_mm_arr_len = 0;
	spin_lock_init(&emp_mm_arr_lock);

	dma_init();

	//nvme_test_while_init(); /* for debug */
	//
	initial_local_cache_pages = LOCAL_CACHE_PAGES;
#ifdef CONFIG_EMP_BLOCK
	initial_block_order = BLOCK_MAX_ORDER;
	initial_subblock_order = 0;
	initial_critical_subblock_first = true;
	initial_critical_page_first = false;
	initial_mark_empty_page = false;
	initial_mem_poll = false;
	initial_enable_transition_csf = false;
#endif
#ifdef CONFIG_EMP_OPT
	initial_eval_media = false;
	initial_chained_ops = DEFAULT_CHAINED_OPERATION;
	initial_async_invlept = 0;
	initial_writeback_optimization_disable = false;
	initial_eager_writeback = false;
#endif
	initial_remote_reuse = DEFAULT_REMOTE_REUSE;
	initial_remote_policy_subblock = DEFAULT_REMOTE_POLICY_SUBBLOCK;
	initial_remote_policy_block = DEFAULT_REMOTE_POLICY_BLOCK;

	ret = emp_procfs_init();
	if (ret) {
		printk(KERN_ERR "unable to create procfs for emp\n");
		goto err;
	}

	emp_major = register_chrdev(0, EMP_DEVICE_NAME, &emp_fops);
	if (emp_major < 0) {
		printk(KERN_ERR "unable to register %s devs: %d\n",
				EMP_DEVICE_NAME,
				emp_major);
		ret = -1;
		goto err;
	}

	emp_class = emp_class_create(THIS_MODULE, EMP_DEVICE_NAME);
	emp_class->dev_uevent = emp_uevent;

	err_dev = device_create(emp_class, NULL, MKDEV(emp_major, 0),
							NULL, EMP_DEVICE_NAME);
	printk(KERN_INFO "%s: register device at major %d\n", EMP_DEVICE_NAME,
			emp_major);

	return 0;

err:
	emp_procfs_exit();
	emp_kfree(emp_mm_arr);
	return ret;
}

/**
 * emp_exit - Exit EMP module
 *
 * Destroy EMP module structures
 */
static void __exit emp_exit(void)
{
#ifdef CONFIG_EMP_VM
	struct emp_mod emp_mod;
	memset(&emp_mod, 0, sizeof(struct emp_mod));
#endif /* CONFIG_EMP_VM */

	if (emp_mm_arr_len != 0) {
		printk(KERN_ERR "unable to unregister %s: %d\n",
				EMP_DEVICE_NAME,
				emp_major);
		return;
	}


	/* workaround code for preventing from system crash
	 * due to modules_exit while the emp is released  */
	mutex_lock(&emp_open_mutex);
	mutex_unlock(&emp_open_mutex);

	if (emp_mm_arr)
		emp_kfree(emp_mm_arr);
	dma_exit();

	device_destroy(emp_class, MKDEV(emp_major,0));
	class_destroy(emp_class);
	unregister_chrdev(emp_major, EMP_DEVICE_NAME);

#ifdef CONFIG_EMP_VM
	register_emp_mod(&emp_mod);
#endif /* CONFIG_EMP_VM */
	kernel_symbol_close();
	emp_procfs_exit();

	emp_debug_alloc_exit();
	printk(KERN_INFO "%s: unregister device at major %d\n", EMP_DEVICE_NAME,
			emp_major);
}

module_init(emp_init)
module_exit(emp_exit)
MODULE_LICENSE("GPL");
