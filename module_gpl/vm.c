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
static inline void finish_emp_vma_split(struct emp_vmr *vmr, const bool locked);
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
	APPLY(migrate_local_page);
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
#ifdef CONFIG_EMP_DEBUG_PF_HISTORY
		if (v->pf_history)
			emp_kfree(v->pf_history);
#endif
#ifdef CONFIG_EMP_OPT
		if (v->eager_wbr_cache)
			emp_kmem_cache_destroy(v->eager_wbr_cache);
#endif /* CONFIG_EMP_OPT */
	}
	emp_list_unlock(free_page_list);

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
#ifdef CONFIG_EMP_DEBUG_PF_HISTORY
		if (v->pf_history)
			emp_kfree(v->pf_history);
#endif
#ifdef CONFIG_EMP_OPT
		if (v->eager_wbr_cache)
			emp_kmem_cache_destroy(v->eager_wbr_cache);
#endif /* CONFIG_EMP_OPT */
	}
	emp_list_unlock(free_page_list);

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

static int emp_vmr_find_and_set(struct emp_mm *emm, struct emp_vmr *vmr)
{
	unsigned long p;
	p = find_first_bit(emm->vmrs_bitmap, EMP_VMRS_MAX);
	if (unlikely(p == EMP_VMRS_MAX))
		return -1;

	vmr->id = p;
	emm->vmrs[p] = vmr;
	__clear_bit(p, emm->vmrs_bitmap);
	emm->vmrs_len++;

	return p;
}

static void emp_vmr_release(struct emp_vmr *vmr)
{
	struct emp_mm *emm = vmr->emm;

	vmr->magic = 0; // remove the magic value
#ifdef CONFIG_EMP_USER
	debug_assert(vmr->mmu_notifier == NULL);
#endif

	if (emm->last_vmr == vmr)
		emm->last_vmr = NULL;

	emm->vmrs[vmr->id] = NULL;
	__set_bit(vmr->id, emm->vmrs_bitmap);
	emm->vmrs_len--;
}

static void emp_vma_close(struct vm_area_struct *vma)
{
	struct emp_vmr *vmr;

	vmr = __get_emp_vmr(vma);
	if (vmr == NULL)
		return;

#ifdef CONFIG_EMP_USER
	/* TODO: we may call finish_emp_vam_split early,
	 *       on gpas_close() from mmu notifier. */
	finish_emp_vma_split(vmr, false);
#endif

	printk(KERN_NOTICE "%s emm: %d num_vmr: %d vmr: %d vma:%016lx virt: %016lx "
				"vmr: %016lx desc: %016lx ref: %d\n",
			__func__, vmr->emm->id, vmr->emm->vmrs_len, vmr->id,
			(unsigned long) vma, vma->vm_start, (unsigned long) vmr,
			(unsigned long) vmr->descs,
			(int) (vmr->descs ? atomic_read(&vmr->descs->refcount) : -1));

	/* vmr->vmr_closing may be set by mmu notifier */
	if (vmr->vmr_closing == false) {
		vmr->vmr_closing = true;
		smp_mb();
		debug_show_gpa_state(vmr, __func__);
#ifdef CONFIG_EMP_USER
#ifdef CONFIG_EMP_DEBUG
		if (vmr->dup_parent)
			debug_show_gpa_state(vmr->dup_parent, "emp_vma_close(parent)");
#endif
#endif /* CONFIG_EMP_USER */
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

	vmr->emm->last_mm = vma->vm_mm;
	vmr->host_vma = NULL;
	vmr->host_mm = NULL;
	emp_kfree(vmr);
}

#ifdef CONFIG_EMP_USER
#define set_gpa_dir(vmr, gpa_dir, idx, new) ({ \
	struct emp_gpa *____g = (struct emp_gpa *) atomic64_cmpxchg( \
						(atomic64_t *) &((gpa_dir)[idx]), \
						(s64) NULL, (s64) (new)); \
	if (unlikely(____g != NULL)) \
		dprintk_ratelimited(KERN_ERR "%s(%s:%d): race at set_gpa_dir() " \
				"is detected. vmr: %d index: 0x%lx " \
				"new: 0x%016lx prev: 0x%016lx\n", \
				__func__, __FILE__, __LINE__,(vmr)->id, (idx), \
				(unsigned long) (new), (unsigned long) ____g); \
	____g; \
})

#define replace_gpa_dir(vmr, gpa_dir, idx, old, new) do { \
	struct emp_gpa *____prev, *____old_head; \
	____old_head = emp_get_block_head(old); \
	debug_assert(____emp_gpa_is_locked(____old_head)); \
	____prev = (struct emp_gpa *) atomic64_cmpxchg( \
					(atomic64_t *) &((gpa_dir)[idx]), \
					(s64) (old), (s64) (new)); \
	if (unlikely(____prev != (old))) \
		printk_ratelimited(KERN_ERR "%s(%s:%d): race at replace_gpa_dir() " \
				"is detected. vmr: %d index: 0x%lx " \
				"old: 0x%016lx new: 0x%016lx prev: 0x%016lx\n", \
				__func__, __FILE__, __LINE__, (vmr)->id, (idx), \
				(unsigned long) old, (unsigned long) new, \
				(unsigned long) ____prev); \
} while (0)

static bool split_reduce_block(struct emp_mm *bvma, struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr, struct emp_gpa *s, unsigned long index_head, struct emp_gpa *hs[])
{
	struct emp_vmr *vmr;
	struct emp_gpa *g, *r;
	bool dirty_block;
	unsigned int flag;
	int sb_index, num_sb;
	unsigned long index_gpa_dir;
	unsigned long block_size;
	int gs_len = 1 << gpa_desc_order(s);
	struct emp_gpa *gs[gs_len];
	int next_order, sb_order, block_order, desc_order;
	struct kmem_cache *cachep;
	struct vcpu_var *cpu = emp_this_cpu_ptr(bvma->pcpus);

	dprintk("%s head index = %ld, flag = 0x%x\n", __func__, index_head, get_gpa_flags(s));

	next_order = gpa_subblock_order(s);
	sb_order = gpa_subblock_order(s);
	block_order = gpa_block_order(s);
	desc_order = block_order - bvma_subblock_order(bvma);
	cachep = get_gpadesc_alloc(prev_vmr->emm, desc_order);

	if (sb_order == block_order) {
		// don't need to reduce block
		hs[0] = s;
		return false;
	}

	if (s->local_page) {
		vmr = bvma->vmrs[s->local_page->vmr_id];
	} else {
		vmr = prev_vmr;
	}

	// remove s from lru
	switch(s->r_state) {
	case GPA_ACTIVE: 
	case GPA_INACTIVE:
#ifdef CONFIG_EMP_EXT
		emp_ops.remove_gpa_from_lru(bvma, s);
#else
		remove_gpa_from_lru(bvma, s);
#endif
		break;
	case GPA_WB:
	default:
		break;
	}

	// lock all subblock
	for_each_gpas(g, s) {
		if (g == s) {
			continue;
		}
		__emp_lock_block(g);
	}

	flag = get_gpa_flags(s);
	dirty_block = flag & GPA_DIRTY_MASK;

	// all the block headers are locked
	num_sb = num_subblock_in_block(s);
	for (g = s, sb_index = 0; g < (s + num_sb); g++, sb_index++) {
		gs[sb_index] = g;

		// update block_order for every member in the block
		set_gpa_block_order(g, next_order);
		if (g == s || g->local_page == NULL)
			continue;

		// update flag, r_state for the header of each subblock
		set_gpa_flags(g, flag);
		g->r_state = s->r_state;
		g->local_page->sptep = s->local_page->sptep + ((g - s) << sb_order);
		g->local_page->gpa_index = index_head + sb_index;;
	}

	for (sb_index = 0, index_gpa_dir = index_head; sb_index < num_sb; sb_index++, index_gpa_dir++) {
		/* allocate gpadesc of which desc_order is 0 */
		r = alloc_gpadesc(prev_vmr->emm, 0);
		if (unlikely(!r)) {
			printk(KERN_ERR "ERROR: cannot allocate gpa descriptor. "
				"emm: %d desc_order: %d\n",
				bvma->id, 0);
			goto reduce_fail;
		}

		g = gs[sb_index];
		memcpy(r, g, sizeof(struct emp_gpa));

		// upate local_page
#ifdef CONFIG_EMP_DEBUG
		r->local_page->gpa = r;
#endif
		r->local_page->gpa_index = index_gpa_dir;;

		// store r in hs array which is used to unlock
		hs[sb_index] = r;

		dprintk("%s BEFORE: index_gpa_dir = %ld new gpa = %p, old gpa = %p, new_gpa->local_page = %p old_gpa->local_page = %p, block_order = %d subblock_order = %d\n", __func__, index_gpa_dir, r, g, r->local_page, g->local_page, gpa_block_order(r), gpa_subblock_order(r));
		/* set r to gpa_dir */
		new_vmr->descs->gpa_dir[index_gpa_dir] = r;
		prev_vmr->descs->gpa_dir[index_gpa_dir] = r;
		//replace_gpa_dir(new_vmr, new_vmr->descs->gpa_dir, index_gpa_dir, g, r);
		//replace_gpa_dir(prev_vmr, prev_vmr->descs->gpa_dir, index_gpa_dir, g, r);
		debug_assert(new_vmr->descs->gpa_dir[index_gpa_dir] == r);
		debug_assert(prev_vmr->descs->gpa_dir[index_gpa_dir] == r);
		dprintk("%s  AFTER: index_gpa_dir = %ld new dir = %p, old dir = %p\n", __func__, index_gpa_dir, new_vmr->descs->gpa_dir[index_gpa_dir], prev_vmr->descs->gpa_dir[index_gpa_dir]);
	}

	// add  all blocks to lru
	switch(s->r_state) {
	case GPA_ACTIVE: 
		for (sb_index = 0; sb_index < num_sb; sb_index++) {
			if (!hs[sb_index]->local_page)
				continue;
			dprintk("%s add to active: gpa = %p, sb_index = %d\n", __func__, hs[sb_index], sb_index);
			block_size = gpa_block_size(hs[sb_index]);
			debug_assert(hs[sb_index]->local_page->vmr_id >= 0
				&& hs[sb_index]->local_page->vmr_id < EMP_VMRS_MAX
				&& bvma->vmrs[hs[sb_index]->local_page->vmr_id] != NULL);
#ifdef CONFIG_EMP_EXT
			emp_ops.update_lru_lists(bvma, cpu, &hs[sb_index], 1, block_size);
#else
			update_lru_lists(bvma, cpu, &hs[sb_index], 1, block_size);
#endif
		}
		break;
	case GPA_INACTIVE:
		for (sb_index = 0; sb_index < num_sb; sb_index++) {
			if (!hs[sb_index]->local_page)
				continue;
			dprintk("%s add to inactive: gpa = %p, sb_index = %d\n", __func__, hs[sb_index], sb_index);
#ifdef CONFIG_EMP_EXT
			emp_ops.add_gpas_to_inactive(bvma, cpu, &hs[sb_index], 1);
#else
			add_gpas_to_inactive(bvma, cpu, &hs[sb_index], 1);
#endif
		}
		break;
	case GPA_WB:
	default:
		break;
	}

	// free the old block
	emp_kmem_cache_free(cachep, gs[0]);

	return true;

reduce_fail:
	{
		struct emp_gpa *e = g;
		for (g = s + 1; g < e; g++)
			__emp_unlock_block(g);
	}

	return false;
}

static inline void split_sort_boundaries(unsigned long *boundary, int num_boundary)
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
static inline void split_sort_regions(struct gpadesc_region *regions, int num_region)
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

#ifdef CONFIG_EMP_VM
static unsigned long split_get_num_low_memory_pages(struct emp_mm *e)
{
	int low_memory_pages = LOW_MEMORY_REGION_SIZE >> PAGE_SHIFT;
	debug_assert(e->ekvm.kvm != NULL);
	if (bvma_block_size(e) > low_memory_pages)
		return bvma_block_size(e);
	else
		return low_memory_pages;
}
#endif

static void
split_set_gpadesc_regions(struct emp_vmr *vmr)
{
	struct emp_mm *emm = vmr->emm;
	struct emp_vmdesc *desc = vmr->descs;
	struct gpadesc_region *regions = desc->regions;
	unsigned long gpa_len = desc->gpa_len;
	unsigned long gpa_len_vm_base, gpa_offset;
	unsigned long boundary[GPADESC_MAX_REGION];
	int num_boundary, i, num_region;
	u8 b_order, sb_order;
	unsigned long block_aligned_start, block_aligned_end;
	unsigned long vpn_start, vpn_end, vpn_base, index_start;
#ifdef CONFIG_EMP_USER
	bool partial_at_head, partial_at_tail;
#endif
#ifdef CONFIG_EMP_VM
	u8 low_order;
	unsigned long low_memory_end;
#endif
	unsigned long vm_start, vm_end, vm_base;
	unsigned long sb_at_head; // number of subblocks which are not block-aligned at head
	unsigned long sb_at_tail; // number of subblocks which are not block-aligned at tail
	unsigned long va_sb_order;

	/* start index in gpa_dir */
	vpn_start = vmr->vm_start >> PAGE_SHIFT;
	vpn_end = vmr->vm_end >> PAGE_SHIFT;
	vpn_base = vmr->descs->vm_base >> PAGE_SHIFT;
	index_start = (vpn_start - vpn_base) >> bvma_subblock_order(emm);

	va_sb_order = bvma_va_subblock_order(emm);

	vm_start = VA_ROUND_DOWN_ORDER(vmr->vm_start, va_sb_order);
	vm_end = VA_ROUND_UP_ORDER(vmr->vm_end, va_sb_order);

	vm_base = VA_ROUND_DOWN_ORDER(vmr->descs->vm_base, va_sb_order);

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
	gpa_len_vm_base = (vm_end - vm_base) >> va_sb_order;
	gpa_offset = gpa_len_vm_base - gpa_len;

	might_sleep();

	/********************************************************/
	/* set gpadesc region					*/
	/********************************************************/

	/* init region */
	num_region = vmr->descs->num_region;
	for (i = 0; i < num_region; i++) {
		struct gpadesc_region *r = &regions[i];
		memset(r, 0, sizeof(struct gpadesc_region));
	}

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
		low_memory_end = split_get_num_low_memory_pages(emm) >> sb_order;
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
	split_sort_boundaries(boundary, num_boundary);

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
	split_sort_regions(regions, num_region);

	/* set desc */
	desc->num_region = num_region;
	desc->gpa_dir = &desc->gpa_dir[index_start];
	desc->gpa_len = gpa_len;
	desc->vm_base = vm_start;
	desc->block_aligned_start = block_aligned_start;

	for (i = 0; i < gpa_len; i++) {
		if (desc->gpa_dir[i] && desc->gpa_dir[i]->local_page)
			desc->gpa_dir[i]->local_page->gpa_index = i;
	}

#ifdef CONFIG_EMP_DEBUG
	for (i = 0; i < num_region; i++) {
		struct gpadesc_region *r = &regions[i];
		printk(KERN_INFO "%s: emm(%d) vmr(%d) region(%d) "
					"start: %ld end: %ld "
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
#ifdef CONFIG_EMP_USER
					r->partial_map ? 1 : 0
#else
					-1
#endif
					);
	}
#endif
}

static void split_update_pte(struct vm_area_struct *vma, struct page *page,
			pmd_t *pmd, unsigned long addr, unsigned long len)
{
	spinlock_t *ptl;
	pte_t pte_entry;
	pte_t *_pte, *pte;
	unsigned long i;

	// ptl is spinlock of pmd page
	ptl = pte_lockptr(vma->vm_mm, pmd);
	pte = emp_pte_map(pmd, addr);
	emp_set_page_mapping_and_index(vma, addr, page);

	spin_lock(ptl);
	/* change the pages */
	for (i = 0, _pte = pte;
			i < len; i++, _pte++, page++, addr += PAGE_SIZE) {
		/* Clear the pte entry and flush it first.
		 * Refer to __wp_page_copy() in the kernel */
		flush_icache_page(vma, page);
		/* TODO: we call ptep_clear_flush() for each page, and thus call
		 *       flush_tlb_page() for each page. This is not efficient.
		 *       Reducing tlb flush here is our future work.
		 *       Refer to unmap_ptes() and __unmap_ptes().
		 */
		kernel_ptep_clear_flush(vma, addr, _pte);

		/*
		   following mk_pte could be a problem without compiler optimization
		   with turning off the optimization of gcc (GCC) 4.4.7 20120313 (Red Hat 4.4.7-4)
		   there was a case that variable assignment of a mk_pte did not work correctly
		 */
		pte_entry = mk_pte(page, vma->vm_page_prot);
		//pte_entry = maybe_mkwrite(pte_mkdirty(pte_entry), vma);
		kernel_page_add_file_rmap(page, vma, false);
		update_mmu_cache(vma, addr, _pte);
		set_pte_at(vma->vm_mm, addr, _pte, pte_entry);
	}
	emp_pte_unmap(pte);
	spin_unlock(ptl);
}

static inline void split_copy_pages(struct page *dst, struct page *src, int offset, int len)
{
	struct page *_dst, *_src;
	void *from, *to;
	int i;

	dprintk("%s offset = %d len = %d\n", __func__, offset, len);

	preempt_disable();
	pagefault_disable();
	for (i = 0, _dst = dst, _src = src + offset; i < len; i++, _dst++, _src++) {
		from = page_address(_src);
		to = page_address(_dst);
		copy_page(to, from);
	}
	pagefault_enable();
	preempt_enable();
}

/* split_local_page
 * create new local_page for back_vmr,
 * copy pages which are involved in back_vmr,
 * and attach the pages to the local_page.
 */
static unsigned long split_local_page(struct emp_vmr *front_vmr, struct emp_vmr *back_vmr, unsigned long split_index, pmd_t *pmd) 
{
	struct emp_mm *emm = back_vmr->emm;
	struct emp_vmdesc *back_desc = back_vmr->descs;
	struct emp_gpa *front_gpa, *back_gpa;
	struct page *src_page;
	struct page *dst_page;
	unsigned long front_hva;
	unsigned long back_hva;
	unsigned long offset;
	unsigned long front_page_len;
	unsigned long back_page_len;
	unsigned long sb_page_len;
	unsigned long vm_start, vm_end;
	unsigned long va_sb_order;
	bool partial_at_head, partial_at_tail;
	struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);

	// get gpa
	front_gpa = front_vmr->descs->gpa_dir[split_index];
	back_gpa = back_desc->gpa_dir[split_index];

	va_sb_order = bvma_va_subblock_order(emm);
	vm_start = VA_ROUND_DOWN_ORDER(back_vmr->vm_start, va_sb_order);
	vm_end = VA_ROUND_UP_ORDER(back_vmr->vm_end, va_sb_order);

	partial_at_head = back_vmr->vm_start != vm_start ? true : false;
        partial_at_tail = back_vmr->vm_end != vm_end ? true : false;

	if (!back_gpa->local_page && partial_at_head) {
		set_gpa_flags_if_unset(front_gpa, GPA_PARTIAL_MAP_MASK);
		set_gpa_flags_if_unset(back_gpa, GPA_PARTIAL_MAP_MASK);
		return 0;
	}

	offset = 0;
	if (unlikely(__is_gpa_flags_set(back_gpa, GPA_PARTIAL_MAP_MASK))) { /* gpa is partial map */
		front_hva = GPN_OFFSET_TO_HVA(front_vmr, split_index, gpa_subblock_order(front_gpa));
		back_hva = GPN_OFFSET_TO_HVA(back_vmr, split_index, gpa_subblock_order(back_gpa));

		front_page_len = ____partial_gpa_to_page_len(front_vmr, front_gpa, split_index, front_hva);
		back_page_len = ____partial_gpa_to_page_len(back_vmr, back_gpa, split_index, back_hva);

		offset = front_page_len;
		set_gpa_flags_if_unset(back_gpa, GPA_PARTIAL_MAP_MASK);
#ifdef CONFIG_EMP_DEBUG
		dprintk("%s: split partial gpa. gpa index = %ld, offset = %ld, front pg_len = %ld, back pg_len = %ld\n", __func__, split_index, offset, front_page_len, back_page_len);
#endif
	} else {
		if (partial_at_head) {
			front_hva = GPN_OFFSET_TO_HVA(front_vmr, split_index, gpa_subblock_order(front_gpa));
			back_hva = GPN_OFFSET_TO_HVA(back_vmr, split_index, gpa_subblock_order(back_gpa));

			sb_page_len = gpa_subblock_size(back_gpa);
			set_gpa_flags_if_unset(back_gpa, GPA_PARTIAL_MAP_MASK);
			back_page_len = ____partial_gpa_to_page_len(back_vmr, back_gpa, split_index, back_hva);
			offset =  sb_page_len - back_page_len;

			set_gpa_flags_if_unset(front_gpa, GPA_PARTIAL_MAP_MASK);
#ifdef CONFIG_EMP_DEBUG
			dprintk("%s: split gpa. gpa index = %ld, offset = %ld, front pg_len = %ld, back pg_len = %ld\n", __func__, split_index, offset, front_page_len, back_page_len);
#endif
		} else {
			printk(KERN_ERR "%s ERROR: no split in subblock.\n", __func__);
		}
	}

	if (!back_gpa->local_page) {
		return offset;
	}

	// get src page to copy dst page
	src_page =  back_gpa->local_page->page;

	// alloc pages
	dst_page = _alloc_pages(emm, gpa_subblock_order(back_gpa), 0, cpu);
	if (unlikely(IS_ERR_OR_NULL(dst_page))) {
		return offset;
	}

	// copy pages
	split_copy_pages(dst_page, src_page,  offset, back_page_len);

	// alloc local_page
	back_gpa->local_page = emm->lops.alloc_local_page(emm, back_vmr->id,
		                                NULL, dst_page, gpa_subblock_order(back_gpa), split_index, back_gpa);
#ifdef CONFIG_EMP_DEBUG
	back_gpa->local_page->gpa = back_gpa;
#endif
	back_gpa->local_page->gpa_index = split_index;
	back_gpa->local_page->vmr_id = back_vmr->id;

	switch(back_gpa->r_state) {
	case GPA_ACTIVE: 
		// upadate pmd
		debug_lru_set_vmr_id_mark(back_gpa->local_page, back_vmr->id);
		emp_lp_insert_pmd(emm, back_gpa->local_page, back_vmr->id, pmd);
		debug_lru_add_vmr_id_mark(back_gpa->local_page, back_vmr->id);
		debug_page_ref_dup_end(back_gpa->local_page);
		debug_page_ref_mark_map(back_vmr->id, back_gpa->local_page);
        
		// udate pte
		split_update_pte(back_vmr->host_vma, dst_page, pmd, back_hva, back_page_len);
        
		// update LRU lists
#ifdef CONFIG_EMP_EXT
		emp_ops.update_lru_lists(emm, cpu, &back_gpa, 1, gpa_block_size(back_gpa));
#else
		update_lru_lists(emm, cpu, &back_gpa, 1, gpa_block_size(back_gpa));
#endif
		break;
	case GPA_INACTIVE:
	case GPA_WB:
#ifdef CONFIG_EMP_EXT
		emp_ops.add_gpas_to_inactive(emm, cpu, &back_gpa, 1);
#else
		add_gpas_to_inactive(emm, cpu, &back_gpa, 1);
#endif
		break;
	default:
		break;
	}

	return offset;

}

static struct emp_gpa *
__split_gpadesc(struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr,
		struct emp_vmr *front_vmr, struct emp_vmr *back_vmr,
		unsigned long split_addr, struct emp_gpa *split_head,
		unsigned long split_head_index, unsigned long split_index,
		pmd_t *pmd)
{
	struct emp_mm *emm = new_vmr->emm;
	struct emp_gpa *front_gpa, *back_gpa;
	int hs_len = 1 << (bvma_block_order(emm) - bvma_subblock_order(emm));
	struct emp_gpa *hs[hs_len];
	int num_subblock = num_subblock_in_block(split_head);
	int sb_index;
	unsigned long addr;
	unsigned long pg_len = 0;
	unsigned long front_pg_len = 0;
	unsigned long back_pg_len = 0;
	bool reduced = false;

	dprintk("%s new_vmr = %d prev_vmr = %d, front_vmr = %d, back_vmr = %d, split_head_index = %ld, split_index = %ld\n", __func__, new_vmr->id, prev_vmr->id, front_vmr->id, back_vmr->id, split_head_index, split_index);
	// get split gpa's page length
	____gpa_to_hva_and_len(prev_vmr, prev_vmr->descs->gpa_dir[split_index], split_index, addr, pg_len);

	// reduce block
	reduced = split_reduce_block(emm, new_vmr, prev_vmr, split_head, split_head_index, hs);

	/* 
	 * split shared subblock between new_vmr and prev_vmr
	 * split addr == back_vmr->host_vma->vm_start
	 */
	if (split_addr & bvma_va_subblock_mask(emm)) {
		/* 
		 * allocate new gpa, local_page and pages, and set them for back_vmr
		 * front_vmr uses the existing gpa, local_page and pages
		 */
		// allocate gpadesc of which desc_order is 0 for back_vmr
		back_gpa = alloc_gpadesc(emm, 0);
		dprintk("%s alloc_gpadesc: back_gpa = %p split_index = %ld\n", __func__, back_gpa, split_index);
		if (unlikely(!back_gpa)) {
			printk(KERN_ERR "ERROR: cannot allocate gpa descriptor. "
				"emm: %d desc_order: %d\n",
				emm->id, 0);
			goto split_fail;
		}

		// copy old gpa to new gpa
		memcpy(back_gpa, back_vmr->descs->gpa_dir[split_index], sizeof(struct emp_gpa));

		// set new gpa to back_vmr
		back_vmr->descs->gpa_dir[split_index] = back_gpa;

		// alloc back_gpa's local_page and copy its pages
		front_pg_len = split_local_page(front_vmr, back_vmr, split_index, pmd);

		// free back_gpa's remote page value
		set_gpa_remote_page_free(back_gpa);

		/* update front_gpa */
		front_gpa = front_vmr->descs->gpa_dir[split_index];
		//if (front_gpa->local_page) {
		if (ACTIVE_BLOCK(front_gpa)) {
#ifdef CONFIG_EMP_DEBUG
			front_gpa->local_page->gpa = front_gpa;
#endif
			if (front_vmr->id != front_gpa->local_page->vmr_id) {
				pmd = emp_lp_pop_pmd(emm, front_gpa->local_page, back_vmr->id);
				debug_lru_del_vmr_id_mark(front_gpa->local_page, back_vmr->id);

				if (front_gpa->local_page->vmr_id == back_vmr->id) {
					front_gpa->local_page->vmr_id = front_vmr->id;
					debug_lru_set_vmr_id_mark(front_gpa->local_page, front_vmr->id);
				}

				emp_lp_insert_pmd(emm, front_gpa->local_page, front_vmr->id, pmd);
				debug_lru_add_vmr_id_mark(front_gpa->local_page, front_vmr->id);
				debug_page_ref_dup_end(front_gpa->local_page);
				debug_page_ref_mark_map(front_vmr->id, front_gpa->local_page);
			}
		}

		/* update rss: whole page length is alreay added to new_vmr. */
		if (front_pg_len) {
			if (front_vmr == prev_vmr) {
				emp_update_rss_sub_kernel(new_vmr, front_pg_len,
					DEBUG_RSS_ADD_KERNEL_VMA_OPEN,
					new_vmr->descs->gpa_dir[split_index], DEBUG_UPDATE_RSS_SUBBLOCK);
				emp_update_rss_add_kernel(prev_vmr, front_pg_len,
					DEBUG_RSS_ADD_KERNEL_VMA_OPEN,
					prev_vmr->descs->gpa_dir[split_index], DEBUG_UPDATE_RSS_SUBBLOCK);
			} else {
				back_pg_len = pg_len - front_pg_len;
				emp_update_rss_sub_kernel(new_vmr, back_pg_len,
					DEBUG_RSS_ADD_KERNEL_VMA_OPEN,
					new_vmr->descs->gpa_dir[split_index], DEBUG_UPDATE_RSS_SUBBLOCK);
				emp_update_rss_add_kernel(prev_vmr, back_pg_len,
					DEBUG_RSS_ADD_KERNEL_VMA_OPEN,
					prev_vmr->descs->gpa_dir[split_index], DEBUG_UPDATE_RSS_SUBBLOCK);
			}
		}

		// copied back_gpa has locked value
		debug_assert(back_vmr->id == back_gpa->local_page->vmr_id);
		emp_unlock_block(back_gpa);
	}

split_fail:
	if (reduced) {
		// unlock subblocks that are locked in split_reduce_block();
		for (sb_index = 1; sb_index < num_subblock; sb_index++) {
			emp_unlock_block(hs[sb_index]);
		}
	}
	return (hs[0]);
}

static int split_handle_remote_prefetch(struct emp_mm *emm, struct emp_vmr *vmr,
				struct emp_gpa *head, unsigned long idx, struct vcpu_var *cpu) {
	int ret;

	ret = fetch_block(emm, vmr, head, idx, 0, cpu, false, false, false);
	debug_progress(head, ret);
	if (unlikely(ret < 0))
		return ret;
	set_gpa_flags_if_unset(head, GPA_PREFETCHED_BLK_MASK);	
	set_gpa_flags_if_unset(head, GPA_PREFETCH_ONCE_MASK);
	return ret;
}

static void __split_vmdesc(struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr)
{
	struct emp_mm *emm = new_vmr->emm;
	struct mm_struct *new_mm = new_vmr->host_mm;
	struct emp_vmdesc *desc = new_vmr->descs;
	unsigned long head_idx, idx, next_head_idx;
	struct emp_gpa *gpa, *head;
	struct emp_gpa *front_head, *back_head;
	struct emp_gpa *split_head = NULL;
	unsigned long index_start, index_end;
	unsigned long prev_index_start, prev_index_end;
	unsigned long vpn, vpn_base, vpn_start, vpn_end;
	unsigned long prev_vpn_base, prev_vpn_start, prev_vpn_end;
	pmd_t *pmd;
	unsigned long split_index;

#ifdef CONFIG_EMP_DEBUG
	if (prev_vmr->vm_end == new_vmr->vm_start) {
		dprintk("%s prev_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx new_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx, block_aligned_start = %ld, gpa_len = %ld, num_region = %d\n",
				__func__, prev_vmr->id, prev_vmr->vm_start, prev_vmr->vm_end, new_vmr->id, new_vmr->vm_start, new_vmr->vm_end,
				prev_vmr->descs->block_aligned_start, prev_vmr->descs->gpa_len, prev_vmr->descs->num_region);
	} else if (new_vmr->vm_end == prev_vmr->vm_start) {
		dprintk("%s new_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx prev_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx, block_aligned_start = %ld, gpa_len = %ld, num_region = %d\n",
				__func__, new_vmr->id, new_vmr->vm_start, new_vmr->vm_end, prev_vmr->id, prev_vmr->vm_start, prev_vmr->vm_end,
				prev_vmr->descs->block_aligned_start, prev_vmr->descs->gpa_len, prev_vmr->descs->num_region);
	} else {
		printk(KERN_ERR "%s ERROR: prev_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx new_vmr(%d)->vm_start = 0x%lx vm_end = 0x%lx, block_aligned_start = %ld, gpa_len = %ld, num_region = %d\n",
				__func__, prev_vmr->id, prev_vmr->vm_start, prev_vmr->vm_end, new_vmr->id, new_vmr->vm_start, new_vmr->vm_end,
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


	split_head = NULL;
	if (prev_vmr->vm_end == new_vmr->vm_start) { /* [prev_vmr] + [new_wmr] */
		dprintk("%s prev_vmr(%d) prev_index_start = %ld prev_index_end = %ld new_vmr(%d) index_start = %ld index_end = %ld\n",
				__func__, prev_vmr->id, prev_index_start, prev_index_end, new_vmr->id, index_start, index_end);
		// find split block
		if ((raw_get_gpadesc(prev_vmr, prev_index_end - 1)!= NULL)
				&& (raw_get_gpadesc(prev_vmr, index_start) != NULL)) {

			front_head = emp_get_block_head(prev_vmr->descs->gpa_dir[prev_index_end - 1]);
			back_head = emp_get_block_head(prev_vmr->descs->gpa_dir[index_start]);
			if (front_head == back_head) {
				split_head = front_head;
				split_index = index_start;
			}
		}

	} else { /* [new_vmr] + [prev_wmr] */
		dprintk("%s new_vmr(%d) index_start = %ld index_end = %ld prev_vmr(%d) prev_index_start = %ld prev_index_end = %ld\n",
				__func__, new_vmr->id, index_start, index_end, prev_vmr->id, prev_index_start, prev_index_end);
		// find split block
		if ((raw_get_gpadesc(prev_vmr, index_end - 1) != NULL)
				&& (raw_get_gpadesc(prev_vmr, prev_index_start) != NULL)) {

			front_head = emp_get_block_head(prev_vmr->descs->gpa_dir[index_end - 1]);
			back_head = emp_get_block_head(prev_vmr->descs->gpa_dir[prev_index_start]);
			if (front_head == back_head) {
				split_head = front_head;
				split_index = index_end - 1;
			}
		}
	}

	dprintk("%s split head = %p split index = %ld\n", __func__, split_head, split_index);
	if (split_head) {
		switch (split_head->r_state) {
		case GPA_INIT: {
			// pre_fetch remote gpa which is shared between new_vmr and prev_vmr to split
			int r = 0;
			struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);

			dprintk("%s split head in remote. split head = %p split index = %ld\n", __func__, split_head, split_index);
			head_idx = emp_get_block_head_index(prev_vmr, split_index);
			head = emp_lock_block(prev_vmr, NULL, head_idx);
			debug_assert(head = split_head);
			emp_stat_inc(emm, blk_prefetch_remote);

			r = split_handle_remote_prefetch(emm, prev_vmr, head, head_idx, cpu);
			debug_progress(split_head, r);
			if (r >= 0) {
				head->r_state = GPA_ACTIVE;
				set_gpa_flags_if_unset(head, GPA_HPT_MASK);
				// update LRU lists
#ifdef CONFIG_EMP_EXT
				dprintk("%s update lru split_head = %p, split_index = %ld\n",
						__func__, split_head, split_index);
				emp_ops.update_lru_lists(emm, cpu, &head, 1, gpa_block_size(head));
#else
				update_lru_lists(emm, cpu, &head, 1, gpa_block_size(head));
#endif
			} else {
				printk(KERN_ERR "%s ERROR: can not prefetch. split head = %p split index = %ld\n", __func__, split_head, split_index);
			}
			emp_unlock_block(head);
			break;
		}
		case GPA_INACTIVE:
			dprintk("%s split head in inactive. split head = %p split index = %ld\n", __func__, split_head, split_index);
			break;
		case GPA_WB:
			dprintk("%s split head in wb. split head = %p split index = %ld\n", __func__, split_head, split_index);
			break;
		default:
			dprintk("%s split head in default(active). split head = %p split index = %ld\n", __func__, split_head, split_index);
			break;
		}
	}


	//		raw_for_all_gpa_heads_range(prev_vmr, head_idx, head, index_start, index_end)
	head_idx = index_start;
	while(head_idx < index_end) {
		head = get_next_exist_head_gpadesc(prev_vmr, &head_idx);
		if (head == NULL)
			break;

		head = emp_lock_block(prev_vmr, NULL, head_idx);
		next_head_idx = head_idx + num_subblock_in_block(head);

		if (head == split_head) {
			/* wait for prefetch completion */
			if (is_gpa_flags_set(head, GPA_PREFETCHED_BLK_MASK)) {
				dprintk("%s wait for preftech completion. split head = %p split index = %ld\n", __func__, split_head, split_index);
				emm->vops.clear_gpa_prefetched_hpt(emm, prev_vmr, head, head_idx);
				clear_gpa_flags_if_set(head, GPA_PREFETCHED_BLK_MASK);
				clear_gpa_flags_if_set(head, GPA_PREFETCH_ONCE_MASK);
			}

			/* wait for writeback completion and fetch to split*/
			if (WB_BLOCK(head)) {
				int r = 0;
				struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);
				dprintk("%s wait for writeback completion. split head = %p split index = %ld\n", __func__, split_head, split_index);

				/* wait for writeback completion and clear writeback work request */
				debug_progress(head->local_page->w, head);
			        emm->sops.clear_writeback_block(emm, head, head->local_page->w,
					cpu, true, false);

				/* fetch to split */
				emp_stat_inc(emm, blk_prefetch_remote);

				r = split_handle_remote_prefetch(emm, prev_vmr, head, head_idx, cpu);
				debug_progress(split_head, r);
				if (r >= 0) {
					head->r_state = GPA_ACTIVE;
					set_gpa_flags_if_unset(head, GPA_HPT_MASK);
					// update LRU lists
#ifdef CONFIG_EMP_EXT
					dprintk("%s update lru split_head = %p, split_index = %ld\n",
							__func__, split_head, split_index);
					emp_ops.update_lru_lists(emm, cpu, &head, 1, gpa_block_size(head));
#else
					update_lru_lists(emm, cpu, &head, 1, gpa_block_size(head));
#endif
					/* wait for prefetch completion */
					emm->vops.clear_gpa_prefetched_hpt(emm, prev_vmr, head, head_idx);
					clear_gpa_flags_if_set(head, GPA_PREFETCHED_BLK_MASK);
					clear_gpa_flags_if_set(head, GPA_PREFETCH_ONCE_MASK);
				} else {
					printk(KERN_ERR "%s ERROR: can not prefetch. split head = %p split index = %ld\n", __func__, split_head, split_index);
				}

			}
		}

		idx = head_idx;
		for_each_gpas(gpa, head) {
			if (unlikely(idx < index_start)) {
				idx++;
				continue;
			}

			if (unlikely(idx >= index_end))
				break;

			/* gpa_dir is initialized with zeros by emp_vzalloc(). */
			set_gpa_dir(new_vmr, desc->gpa_dir, idx, gpa);
			debug_assert(new_vmr->descs->gpa_dir[idx] == gpa);
			idx++;
		}

		if (INIT_BLOCK(head))
			goto next_head;

		vpn = (vpn_start & ~bvma_subblock_mask(emm))
			+ ((head_idx - index_start) << bvma_subblock_order(emm));

		pmd = get_pmd(new_mm, vpn << PAGE_SHIFT);
		idx = head_idx;
		for_each_gpas(gpa, head) {
			if (unlikely(idx < index_start)) {
				idx++;
				continue;
			}
			if (unlikely(idx >= index_end)) {
				break;
			}

			if (ACTIVE_BLOCK(head)) {
				if (!emp_lp_lookup_vmr_id(gpa, prev_vmr->id)) {
					debug_check_page_map_status(new_vmr, head,
								head_idx, pmd, false);
					idx++;
					continue;
				}
                                
				debug_check_page_map_status(new_vmr, head,
								head_idx, pmd, true);
                                
				/* If prev_vmr is mapped, kernel copied the pte to
				 * new_vmr and increased the reference count of the
				 * page. Thus, we have to insert pmd to new_vmr.
				 */
				pmd = emp_lp_pop_pmd(emm, gpa->local_page, prev_vmr->id);
				debug_lru_del_vmr_id_mark(gpa->local_page, prev_vmr->id);
				if (gpa->local_page->vmr_id == prev_vmr->id) {
					gpa->local_page->vmr_id = new_vmr->id;
					debug_lru_set_vmr_id_mark(gpa->local_page, new_vmr->id);
				}
                                
				emp_lp_insert_pmd(emm, gpa->local_page, new_vmr->id, pmd);
				debug_lru_add_vmr_id_mark(gpa->local_page, new_vmr->id);
				debug_page_ref_dup_end(gpa->local_page);
				debug_page_ref_mark_map(new_vmr->id, gpa->local_page); /* mark the kernel's increment on page count */
			} else { // INACTIVE, WB
				if (gpa->local_page->vmr_id == prev_vmr->id) {
					gpa->local_page->vmr_id = new_vmr->id;
					debug_lru_set_vmr_id_mark(gpa->local_page, new_vmr->id);
				}
			}

			/* NOTE: RSS is updated by kernel */
			/*
			emp_update_rss_sub_kernel(prev_vmr, page_len,
				DEBUG_RSS_SUB_KERNEL_COW_MULTI_ACTIVE,
				gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
			*/

			emp_update_rss_add_kernel(new_vmr,
					__local_gpa_to_page_len(new_vmr, gpa),
					DEBUG_RSS_ADD_KERNEL_VMA_OPEN,
					gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
			idx++;
		}

		if (head == split_head) {
			if (prev_vmr->vm_end == new_vmr->vm_start) { /* [prev_vmr] + [new_wmr] */
				head = __split_gpadesc(new_vmr, prev_vmr, prev_vmr, new_vmr, new_vmr->vm_start, split_head, head_idx, index_start, pmd);
			} else { /* [new_vmr] + [prev_wmr] */
				head = __split_gpadesc(new_vmr, prev_vmr, new_vmr, prev_vmr, new_vmr->vm_end, split_head, head_idx, index_end - 1, pmd);
			}
		}

next_head:
		emp_unlock_block(head);
		head_idx = next_head_idx;
	}

	// clear prev_vmr->desc->gpa_dir[] which are not included in prev_vmr
	for (idx = 0; idx < desc->gpa_len; idx++) {
		if (unlikely(idx < prev_index_start))
			prev_vmr->descs->gpa_dir[idx] = NULL;
		if (unlikely(idx >= prev_index_end))
			prev_vmr->descs->gpa_dir[idx] = NULL;
	}
}

static int split_vmdesc(struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr)
{
	struct emp_vmdesc *desc;
	unsigned long gpa_dir_offset;

	dprintk("%s: new: %016lx prev: %016lx \n",
		__func__, (unsigned long) new_vmr, (unsigned long) prev_vmr);

	new_vmr->descs = NULL;

	desc = emp_kzalloc(sizeof(struct emp_vmdesc), GFP_KERNEL);
	if (unlikely(desc == NULL)) {
		printk("%s: ERROR: failed to allocate vm descriptor.\n",
				__func__);
		return -ENOMEM;
	}

	memcpy(desc, prev_vmr->descs, sizeof(struct emp_vmdesc));
#ifdef CONFIG_EMP_USER
	atomic_set(&desc->refcount, 1);
	init_waitqueue_head(&desc->closing_wq);
#endif

	desc->gpa_dir_alloc = emp_vzalloc(desc->gpa_dir_alloc_size);
	if (unlikely(desc->gpa_dir_alloc == NULL)) {
		printk("%s: ERROR: failed to allocate gpa directory. size: %ld\n",
				__func__, desc->gpa_dir_alloc_size);
		emp_kfree(desc);
		return -ENOMEM;
	}

	gpa_dir_offset = ((unsigned long) prev_vmr->descs->gpa_dir)
				- ((unsigned long) prev_vmr->descs->gpa_dir_alloc);
	desc->gpa_dir = desc->gpa_dir_alloc + gpa_dir_offset;

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

	if (emp_vmr_find_and_set(emm, new_vmr) < 0) {
		emp_kfree(new_vmr);
		return NULL;
	}

	new_vmr->magic = EMP_VMR_MAGIC_VALUE;
	new_vmr->emm = emm;
	if (vma)
		__copy_vma_info(new_vmr, vma);
	new_vmr->new_gpadesc = new_gpadesc;
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	new_vmr->set_gpadesc_alloc_at = set_gpadesc_alloc_at;
#endif

#ifdef CONFIG_EMP_USER
	INIT_LIST_HEAD(&new_vmr->dup_shared);
	// new_vmr->dup_parent = NULL due to kzalloc()
	INIT_LIST_HEAD(&new_vmr->dup_children);
	INIT_LIST_HEAD(&new_vmr->dup_sibling);
	/* kzalloc() already gives EMP_FORK_COW; be explicit since the default
	 * decides whether a fork child inherits the parent's backing. */
	new_vmr->fork_policy = EMP_FORK_COW;
#endif
	init_waitqueue_head(&new_vmr->gpas_close_wq);

	return new_vmr;
}

#ifdef CONFIG_EMP_USER
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
		if (emp_get_mmu_notifier(new_vmr))
			return NULL;
	}

	new_vma->vm_private_data = (void *)new_vmr;

	/* A fork child inherits the parent's fork policy, so a grandchild of a
	 * MADV_EMP_WIPEONFORK range is wiped as well. */
	new_vmr->fork_policy = prev_vmr->fork_policy;

	if (vm_shared) {
		new_vmr->descs = prev_vmr->descs;
		atomic_inc(&prev_vmr->descs->refcount);
		dup_list_add(new_vmr, prev_vmr, vm_shared);
		new_vmdesc = false;
		dup_dir = false;
	} else if (vm_wipeonfork) {
		new_vmdesc = true;
		dup_dir = false;
	} else { // map private
		dup_list_add(new_vmr, prev_vmr, vm_shared);
		new_vmdesc = true;
		dup_dir = true;
	}

	printk(KERN_NOTICE "%s emm: %d num_vmr: %d vmr: %d "
			"vma:%016lx virt: %016lx flags: %lx "
			"vmr: %016lx shared: %d wipeonfork: %d\n",
		__func__, emm->id, emm->vmrs_len, new_vmr->id,
		(unsigned long) new_vma, new_vma->vm_start, new_vma->vm_flags,
		(unsigned long) new_vmr, vm_shared, vm_wipeonfork);

	if (dup_vmdesc(new_vmr, prev_vmr, new_vmdesc, dup_dir)) {
		new_vma->vm_private_data = NULL;
		emp_vmr_release(new_vmr);
		new_vmr->host_vma = NULL;
		new_vmr->host_mm = NULL;
		emp_kfree(new_vmr);
		return NULL;
	}

	return new_vmr;
}

static inline void finish_emp_vma_split(struct emp_vmr *vmr, const bool locked)
 {
	if (!locked)
		spin_lock(&vmr->emm->split_link_lock);

	if (vmr->split_new_vmr) {
		struct emp_vmr *split_vmr = vmr->split_new_vmr;
		debug_assert(split_vmr->split_prev_vmr == vmr);
		debug_assert(split_vmr->split_new_vmr == NULL);
		split_vmr->split_prev_vmr = NULL;
#ifdef CONFIG_EMP_DEBUG
		split_vmr->split_addr = 0;
		vmr->split_new_vmr = NULL;
#endif
	}

	if (vmr->split_prev_vmr) {
		struct emp_vmr *split_vmr = vmr->split_prev_vmr;
		debug_assert(split_vmr->split_new_vmr == vmr);
		debug_assert(split_vmr->split_prev_vmr == NULL);
		split_vmr->split_new_vmr = NULL;
#ifdef CONFIG_EMP_DEBUG
		split_vmr->split_addr = 0;
		vmr->split_prev_vmr = NULL;
#endif
	}

	if (!locked)
		spin_unlock(&vmr->emm->split_link_lock);
}

static void __emp_vma_split(struct emp_vmr *prev_vmr, struct emp_vmr *new_vmr,
				struct vm_area_struct *new_vma)
{
	debug_assert(prev_vmr->split_addr = new_vmr->split_addr);
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
	} else {
		debug_assert(new_vmr->vm_start == new_vmr->split_addr);
		debug_assert(prev_vmr->vm_end == new_vmr->vm_end);
		prev_vmr->vm_end = new_vmr->vm_start;
	}

	dprintk("%s  (AFTER) prev: vm_start=%016lx vm_end=%016lx "
		"new_vmr: vm_start=%016lx, vm_end=%016lx\n",
			__func__, prev_vmr->vm_start, prev_vmr->vm_end,
			new_vmr->vm_start, new_vmr->vm_end);

	split_vmdesc(new_vmr, prev_vmr);
	split_set_gpadesc_regions(prev_vmr);
	split_set_gpadesc_regions(new_vmr);

	/* both halves of the split keep the fork policy of the original range */
	new_vmr->fork_policy = prev_vmr->fork_policy;

	new_vmr->vmr_closing = false;
	new_vma->vm_private_data = (void *)new_vmr;
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
				__func__, prev_vmr->emm->id, prev_vmr->id,
				(unsigned long) new_vma);
		new_vmr = __emp_vma_open(prev_vmr, new_vma);
		debug_BUG_ON(!new_vmr);
	}

	vm_flags_set(new_vma, VM_MIXEDMAP | VM_NOHUGEPAGE | VM_DONTEXPAND);

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

	spin_lock(&prev_vmr->emm->split_link_lock);
	finish_emp_vma_split(prev_vmr, true);
	prev_vmr->split_new_vmr = new_vmr;
	new_vmr->split_prev_vmr = prev_vmr;
#ifdef CONFIG_EMP_DEBUG
	prev_vmr->split_addr = addr;
	new_vmr->split_addr = addr;
#endif
	spin_unlock(&prev_vmr->emm->split_link_lock);

	return 0;
}
#endif /* CONFIG_EMP_USER */

static struct vm_operations_struct emp_vma_ops = {
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
			__func__, bvma->id, bvma->vmrs_len,
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

	vma->vm_ops = &emp_vma_ops;
	vma->vm_private_data = (void *)vmr;

#ifdef CONFIG_EMP_VM
	if (bvma->ekvm.kvm && (bvma->ekvm.apic_base_hva == 0UL))
		bvma->ekvm.apic_base_hva = GPN_TO_HVA(bvma, vmr,
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

	bvma->vmrs = emp_kzalloc(sizeof(struct emp_vmr*) * EMP_VMRS_MAX,
				 GFP_KERNEL);
	if (bvma->vmrs == NULL) {
		printk(KERN_ERR "ERROR: failed to allocate vmr pointers (size: %ld)\n",
							sizeof(struct emp_vmr*) * EMP_VMRS_MAX);
		goto err;
	}
	bitmap_fill(bvma->vmrs_bitmap, EMP_VMRS_MAX);

	init_srcu_struct(&bvma->srcu);
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
	if (bvma->vmrs)
		emp_kfree(bvma->vmrs);
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
			__func__, bvma->id, bvma->pid, current->pid, bvma->vmrs_len, emp_mm_arr_len);
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
			__func__, bvma->id, bvma->pid, current->pid, bvma->vmrs_len, emp_mm_arr_len);

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
			__func__, bvma->id, bvma->pid, current->pid, bvma->vmrs_len, emp_mm_arr_len);

	emp_procfs_del(bvma);
	emp_kfree(bvma->vmrs);
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
