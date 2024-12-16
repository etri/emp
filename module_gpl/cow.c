#ifdef CONFIG_EMP_USER
#include <linux/mmu_notifier.h>
#include <linux/mm_types.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include "config.h"
#include "gpa.h"
#include "vm.h"
#include "block.h"
#include "paging.h"
#include "debug.h"
#include "remote_page.h"
#include "donor_mem_rw.h"
#include "glue.h"
#include "block-flag.h"
#include "alloc.h"
#include "reclaim.h"
#include "cow.h"
#include "hva.h"
#include "page_mgmt.h"
#include "pcalloc.h"

/*
 * EMP CoW Handling
 *
 * Entry points
 *
 * ENTRY1: emp_page_fault_hva() with FAULT_FLAG_WRITE
 *  - copy-on-write the faulting vmr.
 *
 * ENTRY2: __wp_page_copy() on EMP managed pages
 *  - do_page_fault()->__handle_mm_fault()->handle_pte_fault()->do_wp_page()
 *    ->__wp_page_copy()->mmu_notifier_invalidate_range()
 *  - copy-on-write the vmr who tries to write.
 *
 * Cases for duplicating gpa descriptors and how to handle them
 *
 * NOTE: @new will belongs to a single vmr. In addition, @old may belongs to
 *       multiple vmrs and some of them may not be mapped. Thus, @new should
 *       not get the remote pages, as unmapped vmrs of @old may need the data.
 *
 * NOTE: @old may not have its remote page since it has never been evicted.
 *       Thus, we can use remote page only when @old has its remote page,
 *       e.g., @old's state is GPA_INIT or GPA_WB. Note that if @old's state
 *       is GPA_INIT and its remote page is invalid, it is zero subblock.
 *       In addtion, if @old's state is GPA_WB, we should wait until its
 *       writeback is finished.
 *
 * CASE1: state=ACTIVE. @vmr is mapped and other vmrs are also mapped.
 *  - ENTRY2: __wp_page_copy()->mmu_notifier_invalidate_range()
 *  - allocate memory pages
 *  - copy memory pages
 *  - make local_page of @new
 *  - move mapped_pmd of @vmr from @old to @new
 *  - update (clear and map) pte (writable)
 *  - remove the remote page (writeback will allocate it)
 *  - decrement ref_count and map_count of old page
 *  - insert @new to the lru chain.
 *  - RESULT: @old and @new are both ACTIVE.
 *
 * CASE2: state=ACTIVE. @vmr is mapped and no other vmrs are mapped.
 *  - ENTRY2: __wp_page_copy()->mmu_notifier_invalidate_range()
 *  - move local_page to @new. (lru elem is moved too.)
 *  - make pte writable
 *  - remove the remote page (writeback will allocate it)
 *  - if @old's remote page is valid,
 *    * change the state of @old to GPA_INIT.
 *    * RESULT: @old is GPA_INIT(remote) and @new is ACTIVE.
 *  - else (@old's remote page is not valid)
 *    * allocate memory pages
 *    * copy memory pages
 *    * make local_page of @old
 *    * insert @old to inactive list
 *    * update lru list
 *    * RESULT: @old is GPA_INACTIVE, and @new is ACTIVE.
 *
 * CASE3: state=ACTIVE. @vmr is not mapped but other vmr is mapped.
 *  - ENTRY1: emp_page_fault_hva() with FAULT_FLAG_WRITE
 *  - allocate memory pages
 *  - copy memory pages
 *  - make local_page of @new
 *  - insert pmd of @vmr to @new
 *  - update (clear and map) pte (writable)
 *  - remove the remote page (writeback will allocate it)
 *  - insert @new to the lru chain.
 *  - RESULT: @old and @new are both ACTIVE.
 *  - NOTE: In this case, installing ptes is not necessary. If we don't install
 *          ptes, emp_page_fault_hva() will install them. However, it requires
 *          inserting @new to inactive list and updating lru lists. Then,
 *          emp_page_fault_hva() moves @new to active list and update lru lists
 *          again. Installing ptes here removes such duplicated overhead.
 *
 * CASE4: state=INACTIVE. no vmrs are mapped.
 *  - ENTRY1: emp_page_fault_hva() with FAULT_FLAG_WRITE
 *  - move local_page to @new. (lru elem is moved too.)
 *  - remove the remote page (writeback will allocate it)
 *  - if @old's remote page is valid,
 *    * change the state of @old to GPA_INIT.
 *    * RESULT: @old is GPA_INIT(remote) and @new is INACTIVE.
 *  - else (@old's remote page is not valid)
 *    * allocate memory pages
 *    * copy memory pages
 *    * make local_page of @old
 *    * insert @old to inactive list
 *    * update lru list
 *    * RESULT: @old and @new are both INACTIVE.
 *
 * CASE5: state=WB. no vmrs are mapped.
 *  - ENTRY1-1: emp_page_fault_hva() with FAULT_FLAG_WRITE
 *  - wait for work_request completion
 *    * NOTE: if we do not wait the completion, update on the local page may
 *      corrupt the remote page, which is @old's data.
 *    * memory page duplication may prevent wating the completion, but it
 *      increases the memory pressure on cold pages (for @old).
 *  - remove the remote page from @new.
 *  - move local_page to @new (likely to be accessed soon)
 *  - move @new to inactive list from writeback list.
 *    * As writeback list is conceptually the part of inactive list, moving
 *      to inactive list removes the need of update lru lists and its length.
 *      In addition, both lists have same mapping state (unmapped).
 *  - change the state of @new to GPA_INACTIVE
 *  - change the state of @old to GPA_INIT(remote)
 *  - RESULT: @old is GPT_INIT(remote), @new is INACTIVE
 *
 * CASE6: state=INIT(remote). no vmrs are mapped.
 *  - ENTRY1-1: emp_page_fault_hva() with FAULT_FLAG_WRITE
 *  - if @old does not have CoW remote page structure, get CoW remote page.
 *  - change the state of @new to GPA_INIT.
 *  - get CoW remote page for @new.
 *  - RESULT: @old and @new are GPA_INIT(CoW).
 *
 * NOTE1: Case that state=INIT(not initialized), e.g., gpa_dir[i] == NULL
 *  - page fault on the gpa will not be handlded as a CoW fault.
 *    The new zero pages are allocated and mapped as other faults.
 *
 * NOTE2: Changes in remote fault handler
 *  - If CoW remote page flag is set, after fetch the remote page,
 *    remove the remote page. Writeback will allocate new remote
 *    page for the dirty local page.
 */

#define emm_desc_order(emm) (bvma_block_order(emm) - bvma_subblock_order(emm))
#define single_mapped_gpa(gpa) ((gpa)->local_page \
				&& EMP_LP_PMDS_SINGLE(&(gpa)->local_page->pmds))
#define gpa_first_mapped_pmd(gpa) ({ \
		debug_assert((gpa)->local_page); \
		(&(gpa)->local_page->pmds); \
})
#define gpa_first_vmr(emm, gpa) ({ \
	debug_assert((gpa)->local_page->pmds.vmr_id >= 0); \
	debug_assert((gpa)->local_page->pmds.vmr_id < (emm)->vmrs_len); \
	debug_assert((emm)->vmrs[(gpa)->local_page->pmds.vmr_id]); \
	(emm)->vmrs[(gpa)->local_page->pmds.vmr_id]; \
})
#define vmr_offset_to_hva(vmr, idx) GPN_OFFSET_TO_HVA(vmr, idx, bvma_subblock_order((vmr)->emm))

#define for_each_old_new_gpas(idx, old, new, head_idx, old_head, new_head) \
		for ((idx) = (head_idx), (old) = (old_head), (new) = (new_head); \
				(idx) < (head_idx) + num_subblock_in_block(old_head); \
				(idx)++, (old)++, (new)++)

#define gpa_page(gpa) ({ \
		debug_assert((gpa)->local_page != NULL); \
		debug_assert((gpa)->local_page->page != NULL); \
		(gpa)->local_page->page; \
})

static inline bool is_gpa_remote_page_valid(struct emp_gpa *gpa)
{
	/* if GPA_TOUCHED_MASK is unset, it has zero pages.
	 * if gpa is on GPA_WB state, the remote page is being modified.
	 */
	return !is_gpa_flags_set(gpa, GPA_TOUCHED_MASK)
		|| (gpa->r_state != GPA_WB && !is_gpa_remote_page_free(gpa));
}

static inline spinlock_t *
__get_pte_lockptr_single(struct emp_mm *emm, struct emp_gpa *head)
{
	struct emp_vmr *prev_vmr = gpa_first_vmr(emm, head);
	struct mapped_pmd *p = gpa_first_mapped_pmd(head);
	debug_assert(single_mapped_gpa(head));
	return pte_lockptr(prev_vmr->host_mm, p->pmd);
}

#define emp_ext_dup_cow_gpa(e, i, o, n) do {} while(0)
#define emp_ext_migrate_local_page(v, o, n) do {} while(0)

static inline void __copy_pages(struct page *dst, struct page *src, int len)
{
	struct page *_dst, *_src;
	void *from, *to;
	int i;
	preempt_disable();
	pagefault_disable();
	for (i = 0, _dst = dst, _src = src; i < len; i++, _dst++, _src++) {
		from = page_address(_src);
		to = page_address(_dst);
		copy_page(to, from);
	}
	pagefault_enable();
	preempt_enable();
}

static inline void __clear_pages(struct page *page, int len)
{
	void *addr = page_address(page);
	memset(addr, 0, PAGE_SIZE * len);
}

static inline void __remove_rmap_on_pages(struct page *page, struct vm_area_struct *vma, unsigned long len)
{
	struct page *_page;
	unsigned long i;
	/* TODO: batched remove rmap? */
	for (i = 0, _page = page; i < len; i++, _page++)
		kernel_page_remove_rmap(_page, vma, false);
}

static inline void __migrate_local_page(struct emp_vmr *vmr,
				struct emp_gpa *old, struct emp_gpa *new)
{
	new->local_page = old->local_page;
	if (likely(new->local_page)) {
		new->local_page->vmr_id = vmr->id;
		debug_lru_set_vmr_id_mark(new->local_page, vmr->id);
		new->local_page->page->private = (unsigned long) new;
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
		new->local_page->gpa = new;
		new->contrib_inactive_len = old->contrib_inactive_len;
		new->contrib_last_file = old->contrib_last_file;
		new->contrib_last_line = old->contrib_last_line;
		new->contrib_last_val = old->contrib_last_val;
		init_gpa_contrib_inactive(old);
#endif
	}
	/* gpa_offset is not changed during duplication */
	old->local_page = NULL;
#ifdef CONFIG_EMP_VM
	/* Since VM does not use fork */
	debug_assert(is_gpa_flags_set(old, GPA_EPT_MASK) == false);
#endif
	if (clear_gpa_flags_if_set(old, GPA_HPT_MASK))
		set_gpa_flags_if_unset(new, GPA_HPT_MASK);
	emp_ext_migrate_local_page(vmr, old, new);
}

static inline void
__set_gpa_remote(struct emp_gpa *gpa)
{
	/* From free_gpa() and cleanup_gpa()
	 * Note that we already remove gpa->local_page */
	clear_gpa_flags_if_set(gpa, GPA_CLEANUP_MASK);
	debug_assert(gpa->local_page == NULL);

	/* From set_gpa_remote() */
	gpa->r_state = GPA_INIT;
	/* gpa->r_state affects the validness of remote page. Thus, we check
	 * the remote page status after the modification of state. */
	debug_assert(is_gpa_remote_page_valid(gpa));
	set_gpa_flags_if_unset(gpa, GPA_REMOTE_MASK);
}

static inline void __set_block_remote(struct emp_gpa *head) {
	struct emp_gpa *gpa;
	for_each_gpas(gpa, head)
		__set_gpa_remote(gpa);
}

static inline void __set_block_state(struct emp_gpa *head, int state) {
	struct emp_gpa *gpa;
	for_each_gpas(gpa, head)
		gpa->r_state = state;
}

static inline int ____add_to_inactive(struct emp_mm *emm,
				struct vcpu_var *cpu, struct emp_gpa *gpa)
{
	return add_gpas_to_inactive(emm, cpu, &gpa, 1);
}

static inline int __add_to_inactive(struct emp_mm *emm,
					struct emp_gpa *gpa)
{
	struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);
	return ____add_to_inactive(emm, cpu, gpa);
}

static inline int __move_to_inactive(struct emp_mm *emm,
				struct vcpu_var *cpu, struct emp_gpa *gpa)
{
	debug_assert(gpa->local_page);
	debug_assert(gpa->local_page->vmr_id >= 0
			&& gpa->local_page->vmr_id < EMP_VMRS_MAX
			&& emm->vmrs[gpa->local_page->vmr_id] != NULL);
	return ____add_to_inactive(emm, cpu, gpa);
}

static inline int
__add_to_writeback(struct emp_mm *emm, struct emp_gpa *gpa)
{
	struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);
	int ret;
	// the number of elements of writeback blocks should be added to
	// inactive_list.page_len, but it is not actually inserted to
	// inactive_list.
	add_inactive_list_page_len(emm, gpa);
	ret = emp_writeback_block(emm, gpa, cpu);
#ifdef CONFIG_EMP_BLOCKDEV
	if (emm->mrs.blockdev_used)
		io_schedule();
#endif
	/* TODO: els_stat_writeback_add() */
	debug_BUG_ON(gpa->r_state != GPA_WB && gpa->r_state != GPA_INIT);
	return ret;
}

static inline struct page *
dup_subblock_pages(struct emp_mm *emm, struct emp_gpa *gpa,
			unsigned long idx, unsigned long page_len,
			struct vcpu_var *cpu, bool is_stale)
{
	struct page *page;
	page = _alloc_pages(emm, gpa->sb_order, 0, cpu);
	if (unlikely(IS_ERR_OR_NULL(page)))
		return page;

	if (unlikely(is_stale)) {
		__clear_pages(page, page_len);
		return page;
	}

	__copy_pages(page, gpa_page(gpa), page_len);
	return page;
}

/* return negative value if error occurs. */
static int COMPILER_DEBUG
get_cow_remote_page(struct emp_mm *emm, struct emp_gpa *old, struct emp_gpa *new)
{
	struct cow_remote_page *crp;
	if (!is_gpa_remote_page_cow(old)) {
		u64 val = __get_gpa_remote_page_val(old);
		crp = emp_kmem_cache_alloc(emm->mrs.cow_remote_pages_cache,
						GFP_KERNEL);
		if (unlikely(crp == NULL))
			return -ENOMEM;
		atomic_set(&crp->refcnt, 1);
		debug_assert((val & REMOTE_PAGE_FLAG_MASK) == 0);
		__set_remote_page(&crp->remote_page, val);
		set_gpa_remote_page_cow(old, crp);
	} else
		crp = get_gpa_remote_page_cow(old);

	atomic_inc(&crp->refcnt);
	set_gpa_remote_page_cow(new, crp);
	return 0;
}

bool COMPILER_DEBUG
__put_cow_remote_page(struct emp_mm *emm, struct emp_gpa *gpa)
{
	struct cow_remote_page *crp = get_gpa_remote_page_cow(gpa);
	if (atomic_dec_and_test(&crp->refcnt)) {
		u64 val = __get_remote_page_val(&crp->remote_page);
		debug_assert((val & REMOTE_PAGE_FLAG_MASK) == 0);
		__set_remote_page(&gpa->remote_page, val);
		emp_kmem_cache_free(emm->mrs.cow_remote_pages_cache, crp);
		return true;
	} else {
		/* initialize remote_page. writeback will re-allocate it. */
		set_gpa_remote_page_free(gpa);
		return false;
	}
}

static void noinline COMPILER_DEBUG
__error_get_cow_remote_page_block(struct emp_mm *emm,
			struct emp_gpa *old_head, struct emp_gpa *new_head,
			int faulting_idx)
{
	struct emp_gpa *old, *new;
	unsigned long idx;

	for_each_old_new_gpas(idx, old, new, 0, old_head, new_head) {
		if (idx == faulting_idx)
			break;
		__put_cow_remote_page(emm, new);
		__put_cow_remote_page(emm, old);
	}
}

static inline int COMPILER_DEBUG
get_cow_remote_page_block(struct emp_mm *emm,
			struct emp_gpa *old_head, struct emp_gpa *new_head)
{
	struct emp_gpa *old, *new;
	unsigned long idx;
	int ret = 0;

	for_each_old_new_gpas(idx, old, new, 0, old_head, new_head) {
		ret = get_cow_remote_page(emm, old, new);
		if (unlikely(ret < 0)) {
			__error_get_cow_remote_page_block(emm,
						old_head, new_head, idx);
			break;
		}
	}

	return ret;
}

#ifdef CONFIG_EMP_DEBUG_SHOW_GPA_STATE
const static char *emp_cow_caller_str[NUM_EMP_COW_CALLER] = {
	"hva_fault",
	"mmu_notifier",
};
#endif

#ifdef CONFIG_EMP_DEBUG_SHOW_GPA_STATE
static void debug_show_gpa_state_cow(struct emp_mm *emm, struct emp_vmr *vmr,
			 enum EMP_COW_CALLER caller, const char *func,
			 struct emp_gpa *old_head, struct emp_gpa *new_head,
			 unsigned long head_idx, unsigned long end_idx)
{
	unsigned long i;
	struct emp_gpa *old, *new;
	struct emp_vmr *old_vmr;
	if (vmr->descs->gpa_len > CONFIG_EMP_DEBUG_SHOW_GPA_STATE)
		return;

	emp_debug_bulk_msg_lock();
	printk(KERN_ERR "[SHOW_GPA] (%d-%d) from: %s(%s)\n",
			emm->id, vmr->id, func, emp_cow_caller_str[caller]);
	for (i = head_idx, new = new_head, old = old_head;
			i < end_idx;
				i++, new = new ? new + 1 : NULL,
				     old = old ? old + 1 : NULL) {
		if (!old)
			goto show_new;

		// find vmr of @old
		if (old->local_page == NULL)
			old_vmr = vmr;
		else if (!EMP_LP_PMDS_EMPTY(&old->local_page->pmds))
			old_vmr = emm->vmrs[old->local_page->pmds.vmr_id];
		else if (old->local_page->vmr_id >= 0)
			old_vmr = emm->vmrs[old->local_page->vmr_id];
		else
			old_vmr = vmr;

		__debug_show_gpa_state(old_vmr, old, i, true);

show_new:
		if (!new)
			continue;

		__debug_show_gpa_state(vmr, new, i, false);
	}
	emp_debug_bulk_msg_unlock();
}
#else
#define debug_show_gpa_state_cow(emm, v, c, f, o, n, h, e) do {} while (0)
#endif

static void __cow_pmd_populate(struct vm_area_struct *vma, pmd_t *pmd, unsigned long hva)
{
	spinlock_t *ptl = pmd_lock(vma->vm_mm, pmd);
	pgtable_t prealloc_pte;

	if (unlikely(!pmd_none(*pmd))) {
		spin_unlock(ptl);
		return;
	}

	prealloc_pte = kernel_pte_alloc_one(vma->vm_mm, hva);

	mm_inc_nr_ptes(vma->vm_mm);
	pmd_populate(vma->vm_mm, pmd, prealloc_pte);
	spin_unlock(ptl);
}

static void COMPILER_DEBUG
__cow_update_pte(struct vm_area_struct *vma, struct page *page,
			pmd_t *pmd, unsigned long addr, unsigned long len)
{
	spinlock_t *ptl;
	pte_t pte_entry;
	pte_t *_pte, *pte;
	unsigned long i;

	// ptl is spinlock of pmd page
	ptl = pte_lockptr(vma->vm_mm, pmd);
	pte = pte_offset_map(pmd, addr);

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
		pte_entry = maybe_mkwrite(pte_mkdirty(pte_entry), vma);
		kernel_page_add_file_rmap(page, vma, false);
		update_mmu_cache(vma, addr, _pte);
		set_pte_at(vma->vm_mm, addr, _pte, pte_entry);
	}
	pte_unmap(pte);
	spin_unlock(ptl);
}

static inline void
cow_update_pte(struct emp_vmr *vmr, struct emp_gpa *head,
			unsigned long addr, unsigned long page_len)
{
	pmd_t *pmd;
	struct emp_gpa *gpa;

	debug_assert(emp_lp_count_pmd(head->local_page) == 1);
	debug_assert(head->local_page->pmds.vmr_id == vmr->id);

	debug_BUG_ON(page_len != (1 << head->sb_order)
			&& head->block_order != head->sb_order);

	pmd = head->local_page->pmds.pmd;
	if (debug_WARN_ONCE(pmd_none(*pmd),
			"WARN: (%s) pmd is none. vmr: %d gpa_idx: 0x%lx "
			"pmd: 0x%016lx addr: 0x%016lx",
			__func__, vmr->id, head->local_page->gpa_index,
			(unsigned long) pmd, addr))
		__cow_pmd_populate(vmr->host_vma, pmd, addr);


	for_each_gpas(gpa, head) {
		debug_assert(emp_lp_count_pmd(gpa->local_page) == 1);
		debug_assert(gpa->local_page->pmds.vmr_id == vmr->id);
		debug_assert(gpa->local_page->pmds.pmd == pmd);

		/* we does not update page_len since partial map gpa block
		 * can have only single subblock. */
		__cow_update_pte(vmr->host_vma, gpa_page(gpa),
					pmd, addr, page_len);
		addr += PAGE_SIZE << gpa->sb_order;
	}

	/* TODO: Batch TLB flushing for block granularity.
	 *       Refer to unmap_ptes() and __unmap_ptes()
	 *       Note that @addr is updated and points the last end address */
}

static void COMPILER_DEBUG
__cow_mkwrite_pte(struct vm_area_struct *vma, struct page *page,
		pmd_t *pmd, unsigned long addr, unsigned long len)
{
	spinlock_t *ptl;
	pte_t pte_entry;
	pte_t *_pte, *pte;
	unsigned long i;
	unsigned long pfn;

	// ptl is spinlock of pmd page
	ptl = pte_lockptr(vma->vm_mm, pmd);
	pte = pte_offset_map(pmd, addr);

	spin_lock(ptl);
	/* make ptes writable */
	for (i = 0, _pte = pte; i < len; i++, _pte++, page++, addr += PAGE_SIZE) {
		/* Following the wp_page_reuse() in the kernel.
		 * However, since we can assume that the pages still
		 * belong to the same process, we skip clearing the
		 * pages cpupid information. */
		pte_entry = *_pte;
		pfn = pte_pfn(pte_entry);
		debug_assert(page_to_pfn(page) == pfn);
		flush_cache_page(vma, addr, pfn);
		pte_entry = pte_mkyoung(pte_entry);
		pte_entry = maybe_mkwrite(pte_mkdirty(pte_entry), vma);
		if (likely(!pte_same(*_pte, pte_entry))) {
			native_set_pte(_pte, pte_entry);
			update_mmu_cache(vma, addr, _pte);
		}
	}

	pte_unmap(pte);
	spin_unlock(ptl);
}

static inline void
cow_mkwrite_pte(struct emp_vmr *vmr, unsigned long head_idx, struct emp_gpa *head)
{
	pmd_t *pmd;
	unsigned long addr, page_len;
	struct emp_gpa *gpa;

	debug_assert(emp_lp_count_pmd(head->local_page) == 1);
	debug_assert(head->local_page->pmds.vmr_id == vmr->id);

	____gpa_to_hva_and_len(vmr, head, head_idx, addr, page_len);
	debug_BUG_ON((page_len != (1 << head->sb_order))
			&& (head->block_order != head->sb_order));

	pmd = head->local_page->pmds.pmd;
	if (debug_WARN_ONCE(pmd_none(*pmd),
			"WARN: (%s) pmd is none. vmr: %d gpa_idx: 0x%lx "
			"pmd: 0x%016lx hva: 0x%016lx",
			__func__, vmr->id, head_idx, (unsigned long) pmd, addr))
		__cow_pmd_populate(vmr->host_vma, pmd, addr);

	for_each_gpas(gpa, head) {
		debug_assert(emp_lp_count_pmd(gpa->local_page) == 1);
		debug_assert(gpa->local_page->pmds.vmr_id == vmr->id);
		debug_assert(gpa->local_page->pmds.pmd == pmd);

		/* we does not update page_len since partial map gpa block
		 * can have only single subblock. */
		__cow_mkwrite_pte(vmr->host_vma, gpa_page(gpa),
						pmd, addr, page_len);
		addr += PAGE_SIZE << gpa->sb_order;
	}
}

static inline int
check_cow_fault_hva(struct emp_vmr *vmr, struct emp_gpa *head)
{
	debug_assert(____emp_gpa_is_locked(head));

	if (atomic_read(&head->refcnt) <= 1)
		return false;

	if (vmr->host_vma->vm_flags & VM_SHARED)
		return false;

	return true;
}

static inline int
check_cow_fault_mmu(struct emp_vmr *vmr, struct emp_gpa *head)
{
	debug_assert(____emp_gpa_is_locked(head));

	if (atomic_read(&head->refcnt) <= 1)
		return false;

	debug_assert((vmr->host_vma->vm_flags & VM_SHARED) == 0);

	/* It is difficult to distinguish CoW fault (write fault on
	 * read-only shared page in private mapping) on mmu notifier.
	 * Instead, we additionally check other possible conditions.
	 * Note that duplication of private mapping does not affect
	 * the correctness.
	 */

	if (unlikely(head->r_state != GPA_ACTIVE))
		return false;

	if (unlikely(head->local_page == NULL))
		return false;

	return true;
}

#ifdef CONFIG_EMP_IO
typedef int (*check_cow_fault_t)(struct emp_vmr *vmr, struct emp_gpa *head);
static check_cow_fault_t check_cow_fault[NUM_EMP_COW_CALLER] = {
	check_cow_fault_hva,
	check_cow_fault_mmu,
};
#endif

static inline struct emp_gpa *
alloc_and_lock_gpadesc(struct emp_mm *emm, int desc_order) {
	int i, len = 1 << desc_order;
	struct emp_gpa *head, *gpa;
	head = alloc_gpadesc(emm, desc_order);
	if (unlikely(!head)) {
		printk(KERN_ERR "ERROR: cannot allocate gpa descriptor. "
				"emm: %d desc_order: %d\n",
				emm->id, desc_order);
		return NULL;
	}
	memset(head, 0, sizeof(struct emp_gpa) * len);
	for (i = 0, gpa = head; i < len; i++, gpa++) {
		____emp_gpa_init_lock(gpa);
		init_progress_info(gpa);
		__emp_lock_block(gpa);
	}
	return head;
}

/* one of src_vmr and dst_vmr can be NULL */
 static inline int
__dup_partial_block_local_page(struct emp_mm *emm, struct emp_vmr *src_vmr,
				struct emp_vmr *dst_vmr, unsigned long idx,
				struct emp_gpa *src, struct emp_gpa *dst)
{
	struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);
	struct page *page;
	unsigned long addr, page_len;
	int dst_vmr_id = dst_vmr ? dst_vmr->id : -1;

	debug_assert(is_gpa_flags_set(src, GPA_PARTIAL_MAP_MASK));
	debug_assert(src->block_order == src->sb_order);

	/* NOTE: src_vmr and dst_vmr have same address range */
	____gpa_to_hva_and_len(dst_vmr ? dst_vmr : src_vmr,
					src, idx, addr, page_len);

	/* duplicate memory pages */
	page = dup_subblock_pages(emm, src, idx, page_len, cpu,
				is_gpa_flags_set(src, GPA_STALE_BLOCK_MASK));
	if (unlikely(IS_ERR_OR_NULL(page))) {
		printk(KERN_ERR "ERROR: %s failed to duplicate memory "
				"pages. emm: %d vmr: %d idx: 0x%lx "
				"err: %ld\n", __func__, emm->id,
				dst_vmr_id, idx, PTR_ERR(page));
		return PTR_ERR(page);
	}

	/* make local_page of @new */
	dst->local_page = emm->lops.alloc_local_page(emm, dst_vmr_id,
				NULL, page, src->sb_order, idx, dst);
	if (unlikely(dst->local_page == NULL)) {
		printk(KERN_ERR "ERROR: %s failed to allocate local "
				"page. emm: %d vmr: %d idx: 0x%lx\n",
				__func__, emm->id, dst_vmr_id, idx);
		_refill_global_free_page(emm, page);
		return -ENOMEM;
	}
	__debug_page_ref_update_page_len(dst->local_page, page_len);
	debug_lru_set_vmr_id_mark(dst->local_page, dst_vmr_id);

	return 0;
}

/* one of src_vmr and dst_vmr can be NULL */
static inline int
__dup_block_local_page(struct emp_mm *emm, struct emp_vmr *src_vmr,
			struct emp_vmr *dst_vmr, unsigned long head_idx,
			struct emp_gpa *src_head, struct emp_gpa *dst_head)
{
	struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);
	unsigned long idx;
	struct emp_gpa *src, *dst;
	struct page *page;
	unsigned long page_len = gpa_subblock_size(src_head);
	bool is_stale = is_gpa_flags_set(src_head, GPA_STALE_BLOCK_MASK);
	int dst_vmr_id = dst_vmr ? dst_vmr->id : -1;
	int ret = 0;

	for_each_old_new_gpas(idx, src, dst, head_idx, src_head, dst_head) {
		/* duplicate memory pages */
		page = dup_subblock_pages(emm, src, idx, page_len, cpu, is_stale);
		if (unlikely(IS_ERR_OR_NULL(page))) {
			ret = PTR_ERR(page);
			printk(KERN_ERR "ERROR: %s failed to duplicate memory "
					"pages. emm: %d vmr: %d idx: 0x%lx "
					"err: %d\n", __func__, emm->id,
					dst_vmr_id, idx, ret);
			goto error;
		}

		/* make local_page of @new */
		dst->local_page = emm->lops.alloc_local_page(emm, dst_vmr_id,
					NULL, page, src->sb_order, idx, dst);
		if (unlikely(dst->local_page == NULL)) {
			printk(KERN_ERR "ERROR: %s failed to allocate local "
					"page. emm: %d vmr: %d idx: 0x%lx\n",
					__func__, emm->id, dst_vmr_id, idx);
			_refill_global_free_page(emm, page);
			ret = -ENOMEM;
			goto error;
		}
		__debug_page_ref_update_page_len(dst->local_page, page_len);
		debug_lru_set_vmr_id_mark(dst->local_page, dst_vmr_id);
	}

	return ret;

error:
	for_each_old_new_gpas(idx, src, dst, head_idx, src_head, dst_head) {
		if (dst->local_page == NULL)
			break;
		page = dst->local_page->page;
		debug_assert(page);
		emm->lops.free_local_page(emm, dst->local_page);
		page->private = 0;
		emp_clear_pg_mlocked(page, dst->sb_order);
		_refill_global_free_page(emm, page);
		dst->local_page = NULL;
	}

	return ret;
}

/* one of src_vmr and dst_vmr can be NULL */
static inline int
dup_block_local_page(struct emp_mm *emm, struct emp_vmr *src_vmr,
			struct emp_vmr *dst_vmr, unsigned long head_idx,
			struct emp_gpa *src_head, struct emp_gpa *dst_head)
{
	debug_assert(src_head->local_page);
	debug_BUG_ON(src_vmr == NULL && dst_vmr == NULL);

	if (unlikely(is_gpa_flags_set(src_head, GPA_PARTIAL_MAP_MASK)))
		return __dup_partial_block_local_page(emm, src_vmr, dst_vmr,
						head_idx, src_head, dst_head);
	else
		return __dup_block_local_page(emm, src_vmr, dst_vmr,
						head_idx, src_head, dst_head);
}

enum DUP_COW_RET {
	DUP_COW_DO_NOTHING = 0,
	DUP_COW_ADD_ACTIVE = 1, // if we need add new head on active list
};

/* dup_cow_gpadesc_* functions
 * Return DUP_COW_ADD_ACTIVE if the fault is handled and we need to insert @new on LRU chain.
 * Return DUP_COW_DO_NOTHING if the fault is handled and there is nothing to do.
 * Return negative value (ERROR CODE) if some error occurs.
 */
static int
dup_cow_gpadesc_multi_active(struct emp_vmr *vmr, unsigned long head_idx,
			struct emp_gpa *old_head, struct emp_gpa *new_head)
{
	struct emp_mm *emm = vmr->emm;
	pmd_t *pmd;
	unsigned long idx;
	struct emp_gpa *old, *new;
	unsigned long addr, page_len;
	int ret;

	____gpa_to_hva_and_len(vmr, old_head, head_idx, addr, page_len);

	// Duplicate old_head's local page to new_head
	ret = dup_block_local_page(emm, NULL, vmr,
					head_idx, old_head, new_head);
	if (unlikely(ret < 0)) {
		printk(KERN_ERR "ERROR: %s failed to duplicate local page. "
				"emm: %d vmr: %d head_idx: 0x%lx\n",
				__func__, emm->id, vmr->id, head_idx);
		return ret;
	}

	for_each_old_new_gpas(idx, old, new, head_idx, old_head, new_head) {
		/* move mapped_pmd from @old to @new */
		pmd = emp_lp_pop_pmd(emm, old->local_page, vmr->id);
		debug_lru_del_vmr_id_mark(old->local_page, vmr->id);
		if (old->local_page->vmr_id == vmr->id) {
			old->local_page->vmr_id = old->local_page->pmds.vmr_id;
			debug_lru_set_vmr_id_mark(old->local_page,
						old->local_page->pmds.vmr_id);
		}
		emp_lp_insert_pmd(emm, new->local_page, vmr->id, pmd);
		debug_lru_add_vmr_id_mark(new->local_page, vmr->id);

		/* NOTE: No vmr's RSS is changed. */
		emp_update_rss_sub_kernel(vmr, page_len,
					DEBUG_RSS_SUB_KERNEL_COW_MULTI_ACTIVE,
					old, DEBUG_UPDATE_RSS_SUBBLOCK);
		emp_update_rss_add_kernel(vmr, page_len,
					DEBUG_RSS_ADD_KERNEL_COW_MULTI_ACTIVE,
					new, DEBUG_UPDATE_RSS_SUBBLOCK);

		/* increase reference count */
		debug_page_ref_will_pte_beg(new->local_page, page_len);
		__emp_get_pages_map(vmr, new, page_len);
		debug_page_ref_will_pte_end(new->local_page, page_len);

		/* we does not update page_len since partial map gpa block
		 * can have only single subblock. */
	}

	/* update (clear and map) pte (writable) */
	cow_update_pte(vmr, new_head, addr, page_len);

	/* NOTE: the remote page is removed at __dup_cow_gpadesc() */

	for_each_gpas(old, old_head) {
		/* decrease ref_count and map_count of old page */
		__remove_rmap_on_pages(gpa_page(old), vmr->host_vma, page_len);
		__emp_put_pages_map(vmr, old, page_len);

		/* we does not update page_len since partial map gpa block
		 * can have only single subblock. */
	}

	return DUP_COW_ADD_ACTIVE;
}

static int
dup_cow_gpadesc_single_active(struct emp_vmr *vmr, unsigned long head_idx,
				struct emp_gpa *old_head, struct emp_gpa *new_head)
{
	unsigned long idx;
	struct emp_gpa *old, *new;

	debug_assert(single_mapped_gpa(old_head));

	for_each_old_new_gpas(idx, old, new, head_idx, old_head, new_head)
		__migrate_local_page(vmr, old, new);

	cow_mkwrite_pte(vmr, head_idx, new_head);

	if (is_gpa_remote_page_valid(old_head)) {
		__set_block_remote(old_head);
	} else {
		struct emp_mm *emm = vmr->emm;
		int ret;

		// Duplicate new_head's local page to old_head
		ret = dup_block_local_page(emm, vmr, NULL,
						head_idx, new_head, old_head);
		if (unlikely(ret < 0)) {
			printk(KERN_ERR "ERROR: %s failed to duplicate local "
					"page. emm: %d vmr: %d head_idx: 0x%lx\n",
					__func__, emm->id, vmr->id, head_idx);
			return ret;
		}

		ret = __add_to_writeback(emm, old_head);
		if (unlikely(ret < 0)) {
			printk(KERN_ERR "ERROR: %s failed to add to inactive list."
					" emm: %d vmr: %d head_idx: 0x%lx\n",
					__func__, emm->id, vmr->id, head_idx);
			return ret;
		}
	}
	return DUP_COW_DO_NOTHING;
}

static int
dup_cow_gpadesc_other_active(struct emp_vmr *vmr, unsigned long head_idx,
				struct emp_gpa *old_head, struct emp_gpa *new_head)
{
	struct emp_mm *emm = vmr->emm;
	unsigned long idx;
	struct emp_gpa *old, *new;
	unsigned long addr, page_len;
	pmd_t *pmd;
	int ret = 0;

	// Duplicate old_head's local page to new_head
	ret = dup_block_local_page(emm, NULL, vmr,
					head_idx, old_head, new_head);
	if (unlikely(ret < 0)) {
		printk(KERN_ERR "ERROR: %s failed to duplicate local page. "
				"emm: %d vmr: %d head_idx: 0x%lx\n",
				__func__, emm->id, vmr->id, head_idx);
		return ret;
	}

	____gpa_to_hva_and_len(vmr, old_head, head_idx, addr, page_len);
	pmd = get_pmd(vmr->host_mm, addr, &pmd);

	for_each_old_new_gpas(idx, old, new, head_idx, old_head, new_head) {
#ifdef CONFIG_EMP_DEBUG
		/* Assert @pmd is same for all subblocks in a block. */
		unsigned long temp;
		debug_assert(pmd == get_pmd(vmr->host_mm,
				vmr_offset_to_hva(vmr, idx), (pmd_t **)&temp));
#endif

		/* insert pmd to @new */
		emp_lp_insert_pmd(emm, new->local_page, vmr->id, pmd);
		debug_lru_add_vmr_id_mark(new->local_page, vmr->id);

		emp_update_rss_add(vmr, page_len, DEBUG_RSS_ADD_COW_OTHER_ACTIVE,
					new, DEBUG_UPDATE_RSS_SUBBLOCK);

		/* increase reference count */
		__emp_get_pages_map(vmr, new, page_len);

		/* we does not update page_len since partial map gpa block
		 * can have only single subblock. */
	}

	emp_update_rss_cached(vmr);

	/* update (clear and map) pte (writable) */
	cow_update_pte(vmr, new_head, addr, page_len);

	return DUP_COW_ADD_ACTIVE;

}

static int
dup_cow_gpadesc_inactive(struct emp_vmr *vmr, unsigned long head_idx,
			struct emp_gpa *old_head, struct emp_gpa *new_head)
{
	unsigned long idx;
	struct emp_gpa *old, *new;

	if (old_head->local_page->vmr_id != vmr->id) {
		struct emp_vmr *old_vmr;
		int page_len;
		debug_assert(old_head->local_page->vmr_id >= 0);
		old_vmr = vmr->emm->vmrs[old_head->local_page->vmr_id];
		page_len = __local_block_to_page_len(old_vmr, old_head);
		emp_update_rss_sub_force(old_vmr, page_len,
						DEBUG_RSS_SUB_COW_INACTIVE,
						old_head, DEBUG_UPDATE_RSS_BLOCK);
		emp_update_rss_add_force(vmr, page_len,
						DEBUG_RSS_ADD_COW_INACTIVE,
						old_head, DEBUG_UPDATE_RSS_BLOCK);
	}

	for_each_old_new_gpas(idx, old, new, head_idx, old_head, new_head)
		__migrate_local_page(vmr, old, new);
	/* new->remote_page was removed at __dup_cow_gpadesc() */

	if (is_gpa_remote_page_valid(old_head)) {
		__set_block_remote(old_head);
	} else {
		struct emp_mm *emm = vmr->emm;
		int ret;

		// Duplicate new_head's local page to old_head
		ret = dup_block_local_page(emm, vmr, NULL,
						head_idx, new_head, old_head);
		if (unlikely(ret < 0)) {
			printk(KERN_ERR "ERROR: %s failed to duplicate local "
					"page. emm: %d vmr: %d head_idx: 0x%lx\n",
					__func__, emm->id, vmr->id, head_idx);
			return ret;
		}

		ret = __add_to_writeback(emm, old_head);
		if (unlikely(ret < 0)) {
			printk(KERN_ERR "ERROR: %s failed to add to inactive list."
					" emm: %d vmr: %d head_idx: 0x%lx\n",
					__func__, emm->id, vmr->id, head_idx);
			return ret;
		}
	}
	return DUP_COW_DO_NOTHING;
}

static int
dup_cow_gpadesc_writeback(struct emp_vmr *vmr, unsigned long head_idx,
			struct emp_gpa *old_head, struct emp_gpa *new_head)
{
	int ret = 0;
	struct emp_mm *emm = vmr->emm;
	struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);
	unsigned long idx;
	struct emp_gpa *old, *new;
	int old_vmr_id = old_head->local_page->vmr_id;
	int page_len = __local_block_to_page_len(vmr, old_head);

	/* wait for writeback completion and clear writeback work request */
	debug_progress(old_head->local_page->w, old_head);
	emm->sops.clear_writeback_block(emm, old_head, old_head->local_page->w,
							cpu, true, false);
	// decrement of inactive_list.page_len is delayed
	sub_inactive_list_page_len(emm, old_head);

	/* NOTE: This is writeback fault. old_vmr_id may be negative. */
	if (old_vmr_id != vmr->id && old_vmr_id >= 0) {
		struct emp_vmr *old_vmr = emm->vmrs[old_vmr_id];
		emp_update_rss_sub_force(old_vmr, page_len,
					DEBUG_RSS_SUB_COW_WRITEBACK,
					old_head, DEBUG_UPDATE_RSS_BLOCK);
	}

	for_each_old_new_gpas(idx, old, new, head_idx, old_head, new_head) {
		/* clear work request pointer */
		old->local_page->w = NULL;
		/* move local page */
		__migrate_local_page(vmr, old, new);
	}

	if (old_vmr_id != vmr->id)
		emp_update_rss_add_force(vmr, page_len,
					DEBUG_RSS_ADD_COW_WRITEBACK,
					new_head, DEBUG_UPDATE_RSS_BLOCK);

	/* move @new from writeback list to inactive list */
	ret = __move_to_inactive(vmr->emm, cpu, new_head);

	if (unlikely(ret < 0)) {
		printk(KERN_ERR "ERROR: %s failed to move to inactive. "
				"err: %d\n", __func__, ret);
		return ret;
	}

	debug_assert(new_head->r_state == GPA_INACTIVE);
	__set_block_remote(old_head);

	/* Writeback list is conceptually the part of inactive list.
	 * Thus, we don't need to update lru list. */
	return DUP_COW_DO_NOTHING;
}

static int
dup_cow_gpadesc_remote(struct emp_vmr *vmr, unsigned long head_idx,
			struct emp_gpa *old_head, struct emp_gpa *new_head)
{
	int ret;
	/* state of old and new is already GPA_INIT */
	debug_assert(old_head->r_state == GPA_INIT);
	debug_assert(new_head->r_state == GPA_INIT);
	ret = get_cow_remote_page_block(vmr->emm, old_head, new_head);
	if (unlikely(ret < 0))
		printk(KERN_ERR "ERROR: %s failed to get cow remote page. "
				"err: %d\n", __func__, ret);
	return ret;
}

static void __dup_cow_gpadesc(struct emp_vmr *vmr, unsigned long head_idx,
			struct emp_gpa *old_head, struct emp_gpa *new_head)
{
	unsigned long idx;
	struct emp_gpa *old, *new;

	for_each_old_new_gpas(idx, old, new, head_idx, old_head, new_head) {
		/* Duplicate the old gpa descriptor.
		 * Refer to __set_gpadesc().
		 * Note that lock and progress_info were initialized
		 * in alloc_and_lock_gpadesc().
		 *
		 * Consideration of flags
		 *
		 * GPA_touched: copy
		 * GPA_dirty: copy
		 * GPA_access: copy
		 * GPA_ept: copy.
		 * GPA_hpt: copy.
		 * GPA_remote: copy. If @old is at remote, @new will be at remote.
		 * GPA_prefetched_csf: must be unset.
		 * GPA_prefetched_cpf: must be unset.
		 * GPA_prefetch_once: copy. This is used to prevent prefetch
		 *                          in the future.
		 * GPA_referenced: copy.
		 * GPA_lowmemory_block: copy.
		 * GPA_stale_block: copy.
		 * GPA_partial_map: copy.
		 * GPA_io_read_page: must be unset.
		 * GPA_io_write_page: must be unset.
		 * GPA_io_in_progress: must be unset.
		 */
		init_gpa_flags(new, get_gpa_flags(old));
		debug_BUG_ON(is_gpa_flags_set(new, GPA_PREFETCHED_CSF_MASK));
		debug_BUG_ON(is_gpa_flags_set(new, GPA_PREFETCHED_CPF_MASK));
#ifdef CONFIG_EMP_IO
		debug_BUG_ON(is_gpa_flags_set(new, GPA_IO_READ_MASK));
		debug_BUG_ON(is_gpa_flags_set(new, GPA_IO_WRITE_MASK));
		debug_BUG_ON(is_gpa_flags_set(new, GPA_IO_IP_MASK));
#endif
#ifdef CONFIG_EMP_BLOCK
		new->block_order = old->block_order;
		new->sb_order = old->sb_order;
#endif
		new->last_mr_id = -1;
		new->r_state = old->r_state;
		set_gpa_remote_page_free(new);
#ifdef CONFIG_EMP_DEBUG
#ifdef CONFIG_EMP_DEBUG_INTEGRITY
		new->tag = old->tag;
#endif
		new->gfn_offset_order = old->gfn_offset_order;
#endif
		emp_ext_dup_cow_gpa(vmr->emm, idx, old, new);
	}
}

/**
 * dup_cow_gpadesc - duplicate CoWed gpa descriptor.
 * @param emm emp_mm data structure
 * @param old_head the old block head gpa descriptor which has previous state
 * @param new_head the new block head gpa descriptor just allocated
 *
 * Return DUP_COW_ADD_ACTIVE if the fault is handled and we need to insert @new on LRU chain.
 * Return DUP_COW_DO_NOTHING if the fault is handled and there is nothing to do.
 * Return negative value (ERROR CODE) if some error occurs.
 */
static int COMPILER_DEBUG
dup_cow_gpadesc(struct emp_vmr *vmr, unsigned long head_idx,
		struct emp_gpa *old_head, struct emp_gpa *new_head)
{
	int ret = 0;

	__dup_cow_gpadesc(vmr, head_idx, old_head, new_head);

	if (old_head->r_state != GPA_INIT) {
		struct mapped_pmd *p, *pp;
		struct local_page *old_lp = old_head->local_page;
		debug_assert(old_lp != NULL);

		if (emp_lp_lookup_pmd(old_lp, vmr->id, &p, &pp)) {
			debug_assert(old_head->r_state == GPA_ACTIVE);
			if (!EMP_LP_PMDS_SINGLE(&old_lp->pmds)) {
				/* CASE1 */
				ret = dup_cow_gpadesc_multi_active(vmr,
						head_idx, old_head, new_head);
			} else {
				/* CASE2 */
				ret = dup_cow_gpadesc_single_active(vmr,
						head_idx, old_head, new_head);
			}
		} else {
			if (!EMP_LP_PMDS_EMPTY(&old_lp->pmds)) {
				/* CASE3 */
				debug_assert(old_head->r_state == GPA_ACTIVE);
				ret = dup_cow_gpadesc_other_active(vmr,
						head_idx, old_head, new_head);
			} else if (old_head->r_state == GPA_INACTIVE) {
				/* CASE4 */
				ret = dup_cow_gpadesc_inactive(vmr,
						head_idx, old_head, new_head);
			} else if (old_head->r_state == GPA_WB) {
				/* CASE5 */
				ret = dup_cow_gpadesc_writeback(vmr,
						head_idx, old_head, new_head);
			} else
				debug_BUG();
		}
	} else { /* old_head->r_state == GPA_INIT */
		/* CASE 6*/
		ret = dup_cow_gpadesc_remote(vmr,
					head_idx, old_head, new_head);
	}

	return ret;
}

/* @retval 1 handle a CoW fault
 * @retval 0 not a CoW fault, but exit normally
 * @retval negative error occurs
 */
static int __handle_emp_cow_fault(struct emp_mm *emm, struct emp_vmr *vmr,
			struct emp_gpa *head, unsigned long head_idx,
			struct vm_fault *vmf, enum EMP_COW_CALLER caller)
{
	int ret = 0;
	int desc_order;
	struct emp_gpa **gpa_dir = vmr->descs->gpa_dir;
	unsigned long i, end_idx, len;
	struct emp_gpa *old_head, *new_head, *old, *new, *demand;
	unsigned int sb_order, demand_off, demand_sb_off;
	struct vcpu_var *cpu;

	/* Assert that demand is the block head */
	debug_assert(head_idx == emp_get_block_head_index(vmr, head_idx));

	/* TODO: support elastic block */
	debug_assert(gpa_block_order(head) ==
			get_gpadesc_region(vmr->descs, head_idx)->block_order);

#ifdef CONFIG_EMP_BLOCK
	/* Support CSF and CPF
	 * - We should wait for fetching whole block and install ptes. */
	cpu = emp_this_cpu_ptr(emm->pcpus);
	if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK)) {
		sb_order = bvma_subblock_order(emm);
		demand_off = head->local_page->demand_offset;
		demand_sb_off = demand_off >> sb_order;
		demand = get_gpadesc(vmr, demand_sb_off);
#ifdef CONFIG_EMP_VM
		/* Since VM does not use fork */
		debug_assert(is_gpa_flags_set(old, GPA_EPT_MASK) == false);
#endif
		if (is_gpa_flags_set(head, GPA_HPT_MASK)) {
			emp_page_fault_hptes_map(emm, vmr, head, demand,
					demand_sb_off - head_idx,
					head, head + num_subblock_in_block(head),
					true, vmf, true);
		}
		clear_gpa_flags_if_set(head, GPA_PREFETCHED_MASK);
#ifdef CONFIG_EMP_STAT
		emm->stat.csf_fault++;
#endif
	}
#endif /* CONFIG_EMP_BLOCK */

	debug_BUG_ON(is_gpa_flags_set(head, GPA_PREFETCHED_CSF_MASK));
	debug_BUG_ON(is_gpa_flags_set(head, GPA_PREFETCHED_CPF_MASK));

#ifdef CONFIG_EMP_IO
	// wait for completion of IO in progress
	// TODO: GPA_IO_IP_MASK may not be set in user-level. Check it later.
	if (is_gpa_flags_set(head, GPA_IO_IP_MASK)) {
		struct page *page;
		struct emp_gpa *gpa = head;
		debug_BUG_ON(!gpa->local_page);
		debug_BUG_ON(!gpa->local_page->page);
		page = gpa->local_page->page;
		emp_unlock_block(gpa);

		wait_on_page_locked(page);

		head = emp_lock_block(vmr, NULL, head_idx);
		debug_BUG_ON(!head); // gpa has existed.
		if (gpa != head) {
			head_idx = emp_get_block_head_index(vmr, head_idx);
			debug_assert(head_idx != WRONG_GPA_IDX); // we already have @gpa
		}
		if (check_cow_fault[caller](vmr, head) == false)
			return 0; /* Not a cow fault anymore */
	}
#endif

	desc_order = gpa_desc_order(head);
	len = 1UL << desc_order;
	end_idx = head_idx + len;
	old_head = head;

	debug_show_gpa_state_cow(emm, vmr, caller,
				"__handle_emp_cow_fault(before)",
				old_head, NULL, head_idx, end_idx);

	debug_assert(head_idx < vmr->descs->gpa_len);
	debug_assert(end_idx <= vmr->descs->gpa_len);
	debug_assert(____emp_gpa_is_locked(head));

	/* Allocate and lock new gpa descriptors */
	new_head = alloc_and_lock_gpadesc(emm, desc_order);
	if (unlikely(new_head == NULL))
		return -ENOMEM;

	/* Duplicate gpa descriptors */
	ret = dup_cow_gpadesc(vmr, head_idx, old_head, new_head);

	/* Add new gpa descriptors to gpa directory of vmr */
	for_each_old_new_gpas(i, old, new, head_idx, old_head, new_head)
		change_gpa_dir(vmr, gpa_dir, i, old, new);

#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	vmr->set_gpadesc_alloc_at(vmr, head_idx, __FILE__, __LINE__);
#endif

	/* Unlock new gpa descriptors except for head */
	for (i = head_idx + 1, new = new_head + 1; i < end_idx; i++, new++)
		__emp_unlock_block(new);

	debug_show_gpa_state_cow(emm, vmr, caller,
				"__handle_emp_cow_fault(after)",
				old_head, new_head, head_idx, end_idx);

	if (unlikely(ret < 0)) {
		if (new_head)
			free_gpadesc(emm, desc_order, new_head);
		return ret;
	}

#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	if (caller == EMP_COW_FROM_MMU_NOTIFIER) {
		/* kernel WILL decrement count on __wp_page_copy().
		 * Mark it before unlock the old gpa */
		unsigned long va = vmf->address;
		unsigned long gpa_idx = (va - vmr->descs->vm_base)
					>> (PAGE_SHIFT + bvma_subblock_order(emm));
		old = old_head + (gpa_idx - head_idx);
		new = new_head + (gpa_idx - head_idx);
		if (old->local_page && old->local_page->page == vmf->page) {
			debug_page_ref_mark(vmr->id, old->local_page, -1);
			debug_page_ref_mmu_noti_end(old->local_page);
		} else if (new->local_page && new->local_page->page == vmf->page) {
			debug_page_ref_mark(vmr->id, new->local_page, -1);
			debug_page_ref_mmu_noti_end(new->local_page);
		}
	}
#endif

	/* Unlock old block head */
	emp_unlock_block(old_head);

	/* Update LRU lists if needed */
	if (ret == DUP_COW_ADD_ACTIVE) {
		struct vcpu_var *cpu = emp_this_cpu_ptr(emm->pcpus);
		unsigned long block_size = gpa_block_size(new_head);
		debug_assert(new_head->local_page->vmr_id >= 0
			&& new_head->local_page->vmr_id < EMP_VMRS_MAX
			&& emm->vmrs[new_head->local_page->vmr_id] != NULL);
		update_lru_lists(emm, cpu, &new_head, 1, block_size);
	}

	return 1;
}

static inline int _handle_emp_cow_fault(struct emp_mm *emm, struct emp_vmr *vmr,
					struct emp_gpa *head, unsigned long idx,
					unsigned long va, enum EMP_COW_CALLER c)
{
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	unsigned long gpa_idx = (va - vmr->descs->vm_base)
					>> (PAGE_SHIFT + bvma_subblock_order(emm));
	struct emp_gpa *gpa = get_gpadesc(vmr, gpa_idx);
#endif
	struct vm_fault vmf = {	.vma = vmr->host_vma,
				.pgoff = GPN_OFFSET(emm, HVA_TO_GPN(emm, vmr, va)),
				.address = va,
				.flags = FAULT_FLAG_WRITE,
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
				.page = gpa->local_page->page,
#endif
				.prealloc_pte = NULL,
	};
	return __handle_emp_cow_fault(emm, vmr, head, idx, &vmf, c);
}

/* @retval 1 handle cow fault
 * @retval 0 not a cow fault
 * @retval negative exit with error.
 */
inline int
handle_emp_cow_fault_hva(struct emp_mm *emm, struct emp_vmr *vmr,
				struct emp_gpa *head, unsigned long idx,
				struct vm_fault *vmf)
{
	debug_assert(idx == emp_get_block_head_index(vmr, idx));
	if (!check_cow_fault_hva(vmr, head))
		return 0;
	else {
		int ret = __handle_emp_cow_fault(emm, vmr, head, idx,
						vmf, EMP_COW_FROM_HVA_FAULT);
		return ret >= 0 ? 1 : ret;
	}
}

/* @retval 0 exit normally.
 * @retval negative exit with error.
 */
static int handle_emp_cow_fault_mmu(struct emp_mm *emm, struct mm_struct *mm,
					struct emp_vmr *vmr, unsigned long va)
{
	unsigned long gpa_idx, head_idx;
	struct emp_gpa *gpa, *head;
	int ret = 0;

	gpa_idx = (va - vmr->descs->vm_base)
			>> (PAGE_SHIFT + bvma_subblock_order(emm));
	gpa = get_gpadesc(vmr, gpa_idx);
	if (unlikely(!gpa))
		return -ENOMEM;

	head = emp_lock_block(vmr, &gpa, gpa_idx);
	debug_BUG_ON(!head); // we already have @gpa
	head_idx = gpa_idx - (gpa - head);

	/* NOTE: During locking, gpa may have gone out and may not have any
	 *       local page. Thus, we use debug_page_ref_*_safe() functions.
	 */

	/* kernel incremented page count on do_wp_page(). Mark it.  */
	debug_page_ref_mmu_noti_beg_safe(gpa->local_page);
	debug_page_ref_mark_safe(vmr->id, gpa->local_page, 1);

	if (atomic_read(&head->refcnt) <= 1) {
		/* This page may be previously duplicated, but it has
		 * no write-permission yet. Just give the permission. */
		if (likely(head->r_state == GPA_ACTIVE && head->local_page)) {
			cow_mkwrite_pte(vmr, head_idx, head);
			debug_page_ref_mark_safe(vmr->id, head->local_page, 0);
		}
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
		else {
			debug_page_ref_mark_safe(vmr->id, head->local_page, 0);
		}
#endif
		goto out;
	}

	if (!check_cow_fault_mmu(vmr, head)) {
		debug_page_ref_mark_safe(vmr->id, head->local_page, 0);
		goto out;
	}

#ifdef CONFIG_EMP_DEBUG_SHOW_GPA_STATE
	if (vmr->descs->gpa_len <= CONFIG_EMP_DEBUG_SHOW_GPA_STATE) {
		emp_debug_bulk_msg_lock();
		printk(KERN_ERR "[EMP_COW_FOUND] emm: %d vmr: %d mm: %016lx "
				"va: %016lx gpa_idx: 0x%lx refcnt: %d\n",
				emm->id, vmr->id, (unsigned long) mm, va,
				gpa_idx, atomic_read(&gpa->refcnt));
		/* state of gpa will be printed out at __handle_emp_cow_fault() */
		emp_debug_bulk_msg_unlock();
	}
#endif

	ret = _handle_emp_cow_fault(emm, vmr, head, head_idx,
				va, EMP_COW_FROM_MMU_NOTIFIER);
	if (ret > 0) {
		/* re-new gpa and head as they may be updated. */
		gpa = get_gpadesc(vmr, gpa_idx);
		if (unlikely(!gpa))
			return -ENOMEM;
		head = emp_get_block_head(gpa);
	}
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	else {
		debug_page_ref_mark_known_diff_safe(vmr->id, gpa->local_page, -1, 1);
	}
#endif

	/* This is write fault, mark it. */
	set_gpa_flags_if_unset(head, GPA_DIRTY_MASK);
 
out:
	/* kernel WILL decrement count on __wp_page_copy().
	 * Mark it before unlock the old gpa */
	debug_page_ref_mmu_noti_end_safe(gpa->local_page);

	emp_unlock_block(head);

	return ret >= 0 ? 0 : ret;
}

#if (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)) \
	|| (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE <= RHEL_RELEASE_VERSION(8, 4))
/* NOTE: sometimes, this function does not work.
 *       find_vma() looks up the *FIRST* vma which satisfies addr < vm_end.
 *       However, sometimes the *FIRST* vma may not what we want.
 *       In the higher version of kernel, it provides vma on mmu notifier,
 *       and we can solve the problem.
 */
static struct emp_vmr *
get_emp_vmr(struct emp_mm *emm, struct mm_struct *mm, unsigned long va)
{
	struct vm_area_struct *vma;
	struct emp_vmr *vmr;

	vma = find_vma(mm, va);
	if (vma == NULL)
		return NULL;

	vmr = __get_emp_vmr(vma);
	if (vmr == NULL)
		return NULL;
	debug_BUG_ON(vmr && !virt_addr_valid(vmr));
	/* Filter the non-emp_vmr */
	if (vmr->magic != EMP_VMR_MAGIC_VALUE
			|| vmr->id >= EMP_VMRS_MAX
			|| emm->vmrs[vmr->id] != vmr)
		return NULL;
	debug_BUG_ON(vmr->host_vma != vma);
#ifdef CONFIG_EMP_DEBUG
	if (unlikely(vmr->emm != emm)) {
		printk(KERN_ERR "ERROR: (%s) vmr(%lx,%d)->emm(%lx,%d) != emm(%lx,%d)\n",
				__func__, (unsigned long)vmr, vmr->id,
					(unsigned long)vmr->emm, vmr->emm->id,
					(unsigned long)emm, emm->id);
		return NULL;
	}
#endif
	return vmr;
}
#endif

static struct emp_vmr *
__get_emp_vmr_check(struct emp_mm *emm, struct vm_area_struct *vma) {
	struct emp_vmr *vmr = __get_emp_vmr(vma);
	if (vmr == NULL)
		return NULL;
	debug_BUG_ON(vmr && !virt_addr_valid(vmr));
	/* Filter the non-emp_vmr */
	if (vmr->magic != EMP_VMR_MAGIC_VALUE
			|| vmr->id >= EMP_VMRS_MAX
			|| emm->vmrs[vmr->id] != vmr)
		return NULL;
	debug_BUG_ON(vmr->host_vma != vma);
#ifdef CONFIG_EMP_DEBUG
	if (unlikely(vmr->emm != emm)) {
		printk(KERN_ERR "ERROR: (%s) vmr(%lx,%d)->emm(%lx,%d) != emm(%lx,%d)\n",
				__func__, (unsigned long)vmr, vmr->id,
					(unsigned long)vmr->emm, vmr->emm->id,
					(unsigned long)emm, emm->id);
		return NULL;
	}
#endif
	return vmr;
}

#ifdef CONFIG_EMP_DEBUG_PAGE_REF
static void __emp_vmr_local_page_dup_beg(struct emp_mm *emm, struct emp_vmr *vmr)
{
	struct emp_gpa *head, *g;
	unsigned long index, index_start, index_end;
	unsigned long vpn_base, vpn_start, vpn_end;
	struct mapped_pmd *mapped, *mapped2;

	vpn_start = vmr->host_vma->vm_start >> PAGE_SHIFT;
	vpn_end = vmr->host_vma->vm_end >> PAGE_SHIFT;
	vpn_base = vmr->descs->vm_base >> PAGE_SHIFT;

	index_start = (vpn_start - vpn_base) >> bvma_subblock_order(emm);
	index_end = (vpn_end - vpn_base + bvma_subblock_size(emm) - 1)
						>> bvma_subblock_order(emm);

	printk(KERN_ERR "[DEBUG] %s: emm: %d vmr: %d addr: %016lx ~ %016lx index: %lx ~ %lx\n",
			__func__, emm->id, vmr->id,
			vmr->host_vma->vm_start, vmr->host_vma->vm_end,
			index_start, index_end);

	raw_for_all_gpa_heads_range(vmr, index, head, index_start, index_end) {
		head = emp_lock_block(vmr, NULL, index);
		debug_BUG_ON(!head); // raw_for_all_gpa_heads_range() iterates only exist heads.

		if (!ACTIVE_BLOCK(head)) {
			emp_unlock_block(head);
			continue;
		}

		for_each_gpas(g, head) {
			if (emp_lp_lookup_pmd(g->local_page, vmr->id,
						&mapped, &mapped2) == false)
				continue;
			debug_page_ref_dup_beg(g->local_page);
			debug_page_ref_mark(vmr->id, g->local_page, 0);
		}

		emp_unlock_block(head);
	}
}

static void __emp_vmr_local_page_unmap_beg(struct emp_mm *emm, struct emp_vmr *vmr)
{
	struct emp_gpa *head, *g;
	unsigned long index, index_start, index_end;
	unsigned long vpn_base, vpn_start, vpn_end;
	struct mapped_pmd *mapped, *mapped2;

	vpn_start = vmr->host_vma->vm_start >> PAGE_SHIFT;
	vpn_end = vmr->host_vma->vm_end >> PAGE_SHIFT;
	vpn_base = vmr->descs->vm_base >> PAGE_SHIFT;

	index_start = (vpn_start - vpn_base) >> bvma_subblock_order(emm);
	index_end = (vpn_end - vpn_base + bvma_subblock_size(emm) - 1)
						>> bvma_subblock_order(emm);

	printk(KERN_ERR "[DEBUG] %s: emm: %d vmr: %d addr: %016lx ~ %016lx index: %lx ~ %lx\n",
			__func__, emm->id, vmr->id,
			vmr->host_vma->vm_start, vmr->host_vma->vm_end,
			index_start, index_end);

	raw_for_all_gpa_heads_range(vmr, index, head, index_start, index_end) {
		head = emp_lock_block(vmr, NULL, index);
		debug_BUG_ON(!head); // raw_for_all_gpa_heads_range() iterates only exist heads.

		if (!ACTIVE_BLOCK(head)) {
			emp_unlock_block(head);
			continue;
		}

		for_each_gpas(g, head) {
			if (emp_lp_lookup_pmd(g->local_page, vmr->id,
						&mapped, &mapped2) == false)
				continue;
			debug_page_ref_unmap_beg(g->local_page);
			debug_page_ref_mark(vmr->id, g->local_page, 0);
		}

		emp_unlock_block(head);
	}
}
#endif

static inline void emp_mmu_noti_unmap(struct emp_vmr *vmr)
{
	vmr->vmr_closing = true;
	smp_mb();
	gpas_close(vmr, true);
}

static void
emp_mmu_notifier_release(struct mmu_notifier *notifier, struct mm_struct *mm)
{
	struct emp_mm *emm = ((struct emp_mmu_notifier *) notifier)->emm;
	struct vm_area_struct *vma;
	struct emp_vmr *vmr;

	dprintk(KERN_ERR "[DEBUG] %s: emm: %d pid: %d\n",
			__func__, emm->id, current->pid);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		vmr = __get_emp_vmr(vma);
		if (vmr == NULL)
			continue;
		/* Filter the non-emp_vmr */
		if (vmr->magic != EMP_VMR_MAGIC_VALUE
				|| vmr->id >= EMP_VMRS_MAX
				|| emm->vmrs[vmr->id] != vmr)
			continue;
		dprintk(KERN_ERR "[DEBUG] %s: emm: %d pid: %d vmr: %d\n",
				__func__, emm->id, current->pid, vmr->id);
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
		__emp_vmr_local_page_unmap_beg(emm, vmr);
#endif
		emp_mmu_noti_unmap(vmr);
	}
}

#if (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)) \
	|| (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE <= RHEL_RELEASE_VERSION(8, 4))
static void
emp_mmu_notifier_invalidate_range_start(struct mmu_notifier *notifier,
		struct mm_struct *mm, unsigned long start, unsigned long end)
{
	struct emp_mm *emm = ((struct emp_mmu_notifier *) notifier)->emm;
	struct emp_vmr *vmr;

	vmr = get_emp_vmr(emm, mm, start);
	if (!vmr)
		return;

	debug_assert(vmr->host_mm == mm);

	if (end - start != PAGE_SIZE)
		return;

	handle_emp_cow_fault_mmu(emm, mm, vmr, start);
}
#else
static int
emp_mmu_notifier_invalidate_range_start(struct mmu_notifier *notifier,
					const struct mmu_notifier_range *range)
{
	int ret = 0;
	struct emp_mm *emm;
	struct emp_vmr *vmr;

#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	if (range->event != MMU_NOTIFY_CLEAR
			&& range->event != MMU_NOTIFY_UNMAP
			&& range->event != MMU_NOTIFY_PROTECTION_PAGE)
		return 0;
#else
	if (range->event != MMU_NOTIFY_CLEAR
			&& range->event != MMU_NOTIFY_UNMAP)
		return 0;
#endif

	emm = ((struct emp_mmu_notifier *) notifier)->emm;

	switch (range->event) {
	case MMU_NOTIFY_CLEAR:
		if (range->end - range->start != PAGE_SIZE)
			break;

		vmr = __get_emp_vmr_check(emm, range->vma);
		if (vmr == NULL)
			break;

		debug_assert(vmr->host_vma == range->vma);
		debug_assert(vmr->host_mm == range->mm);

		ret = handle_emp_cow_fault_mmu(emm, range->mm,
							vmr, range->start);
		break;
	case MMU_NOTIFY_UNMAP:
		vmr = __get_emp_vmr_check(emm, range->vma);
		if (vmr == NULL)
			break;

		debug_assert(vmr->host_vma == range->vma);
		debug_assert(vmr->host_mm == range->mm);

#ifdef CONFIG_EMP_DEBUG_PAGE_REF
		__emp_vmr_local_page_unmap_beg(emm, vmr);
#endif
		emp_mmu_noti_unmap(vmr);
		break;
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	case MMU_NOTIFY_PROTECTION_PAGE:
		vmr = __get_emp_vmr_check(emm, range->vma);
		if (vmr == NULL)
			break;

		if (range->start != vmr->host_vma->vm_start
				|| range->end != vmr->host_vma->vm_end)
			break;
		__emp_vmr_local_page_dup_beg(emm, vmr);
		break;
#endif
	default:
		break;
	}

	return ret;
}
#endif

static struct mmu_notifier *emp_mmu_notifier_alloc(struct mm_struct *mm) {
	return (struct mmu_notifier *) emp_kzalloc(sizeof(struct emp_mmu_notifier), GFP_KERNEL);
}

static void emp_mmu_notifier_free(struct mmu_notifier *notifier) {
	debug_assert(atomic_read(&((struct emp_mmu_notifier *) notifier)->refcnt) == 0);
	emp_kfree(notifier);
}

/* RH_MN_V2 is defined only in RHEL kernels */
#ifndef RH_MN_V2
#define RH_MN_V2(__x) __x
#endif

static struct mmu_notifier_ops emp_mmu_notifier_ops = {
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE <= KERNEL_VERSION(5, 7, 0))
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	.flags                  = 0,
#else
	.flags                  = MMU_INVALIDATE_DOES_NOT_BLOCK,
#endif
#endif
	.RH_MN_V2(invalidate_range_start)
				= emp_mmu_notifier_invalidate_range_start,
	.release                = emp_mmu_notifier_release,
	.alloc_notifier         = emp_mmu_notifier_alloc,
	.free_notifier          = emp_mmu_notifier_free,
};

/* Return 0 if succeed. Otherwise, return the error code. */
long emp_get_mmu_notifier(struct emp_vmr *vmr)
{
	struct emp_mmu_notifier *emp_notifier;
	struct mm_struct *mm = vmr->host_mm;

#ifdef CONFIG_EMP_VM
	/* We don't need this mmu notifier for VMs, and this mmu notifier makes
	 * errors for VMs. */
	debug_assert(vmr->emm->ekvm.kvm == NULL);
#endif

	if (unlikely(vmr->mmu_notifier != NULL))
		return 0; /* already registered */

	dprintk("%s: vmr: %d mm: %016lx vmr->notifier: %016lx\n",
			__func__, vmr->id, (unsigned long) mm,
			(unsigned long) vmr->mmu_notifier);

	emp_notifier = (struct emp_mmu_notifier *) mmu_notifier_get_locked(&emp_mmu_notifier_ops, mm);
	if (IS_ERR_OR_NULL(emp_notifier)) {
		dprintk("[ERROR] EMP failed to get mmu_notifier. emm: %d vmr: %d err: %ld\n",
				vmr->emm->id, vmr->id, PTR_ERR(emp_notifier));
		return PTR_ERR(emp_notifier);
	}

	emp_notifier->emm = vmr->emm;
	vmr->mmu_notifier = emp_notifier;
#ifdef CONFIG_EMP_DEBUG
	atomic_inc(&emp_notifier->refcnt);
#endif
	return 0;
}

void emp_put_mmu_notifier(struct emp_vmr *vmr)
{
	struct emp_mmu_notifier *emp_notifier = vmr->mmu_notifier;

#ifdef CONFIG_EMP_VM
	/* We don't need this mmu notifier for VMs, and this mmu notifier makes
	 * errors for VMs. */
	debug_assert(vmr->emm->ekvm.kvm == NULL);
#endif

	if (unlikely(emp_notifier == NULL))
		return;

	dprintk("%s: vmr->id: %d vmr->notifier: %016lx ->emm: %016lx ->refcnt: %d\n",
			__func__, vmr->id, (unsigned long) vmr->mmu_notifier,
			(unsigned long) emp_notifier->emm,
			atomic_read(&emp_notifier->refcnt));
#ifdef CONFIG_EMP_DEBUG
	atomic_dec(&emp_notifier->refcnt);
#endif
	mmu_notifier_put((struct mmu_notifier *) emp_notifier);
	vmr->mmu_notifier = NULL;

	/* NOTE: Do not put mmu notifier for parent. It may have read-only PTE
	 * that is actually writable. */
}

void dup_list_add(struct emp_vmr *vmr, struct emp_vmr *parent, bool shared)
{
	spin_lock(&vmr->emm->dup_list_lock);

	if (shared) {
		list_add(&vmr->dup_shared, &parent->dup_shared);
	} else { // private
		vmr->dup_parent = parent;
		if (parent)
			list_add(&vmr->dup_sibling, &parent->dup_children);
	}

	spin_unlock(&vmr->emm->dup_list_lock);
}

void dup_list_del(struct emp_vmr *vmr)
{
	struct emp_vmr *child;
	struct emp_vmr *parent = vmr->dup_parent;

	spin_lock(&vmr->emm->dup_list_lock);

	if (!list_empty(&vmr->dup_shared))
		list_del(&vmr->dup_shared);

	list_for_each_entry(child, &vmr->dup_children, dup_sibling)
		child->dup_parent = parent;

	if (parent && !list_empty(&vmr->dup_children))
		list_splice(&vmr->dup_children, &parent->dup_children);

	if (parent)
		list_del(&vmr->dup_sibling);

	spin_unlock(&vmr->emm->dup_list_lock);
}

void __dup_vmdesc(struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr, const bool dup_dir)
{
	struct emp_mm *emm = new_vmr->emm;
	struct mm_struct *new_mm = new_vmr->host_mm;
	struct vm_area_struct *new_vma = new_vmr->host_vma;
	struct emp_vmdesc *desc = new_vmr->descs;
	unsigned long head_idx, idx;
	struct emp_gpa *gpa, *head;
	unsigned long index_start, index_end;
	unsigned long vpn, vpn_base, vpn_start, vpn_end;
	pmd_t *pmd;
	struct mapped_pmd *mapped, *mapped2;

	vpn_start = new_vma->vm_start >> PAGE_SHIFT;
	vpn_end = new_vma->vm_end >> PAGE_SHIFT;
	vpn_base = new_vmr->descs->vm_base >> PAGE_SHIFT;

	index_start = (vpn_start - vpn_base) >> bvma_subblock_order(emm);
	index_end = (vpn_end - vpn_base + bvma_subblock_size(emm) - 1)
						>> bvma_subblock_order(emm);
	/* gpa_dir is initialized with zeros by emp_vzalloc(). */
	raw_for_all_gpa_heads_range(prev_vmr, head_idx, head, index_start, index_end) {
		head = emp_lock_block(prev_vmr, NULL, head_idx);
		debug_BUG_ON(!head); // raw_for_all_gpa_heads() iterates only exist heads.

		if (dup_dir) {
			/* Consideration of elastic block
			 * 1. If @head has been stretched to the next block,
			 *    the next iteration moves @pos to the next of
			 *    stretched block.
			 * 2. If @head has been stretched to the previous block,
			 *    the previous block was already handled and we
			 *    should handle from @pos to the end of the block.
			 */
			idx = head_idx;
			for_each_gpas(gpa, head) {
				set_gpa_dir_new(new_vmr, desc->gpa_dir, idx, gpa);
				idx++;
			}
		}

		if (!ACTIVE_BLOCK(head)) {
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
			vpn = (vpn_start & ~bvma_subblock_mask(emm))
				+ ((head_idx - index_start) << bvma_subblock_order(emm));
			pmd = get_pmd(new_mm, vpn << PAGE_SHIFT, &pmd);
			debug_check_page_map_status(new_vmr, head, head_idx, pmd, false);
#endif
			emp_unlock_block(head);
			continue;
		}

		vpn = (vpn_start & ~bvma_subblock_mask(emm))
			+ ((head_idx - index_start) << bvma_subblock_order(emm));

		pmd = get_pmd(new_mm, vpn << PAGE_SHIFT, &pmd);
		for_each_gpas(gpa, head) {
			if (emp_lp_lookup_pmd(gpa->local_page, prev_vmr->id,
						&mapped, &mapped2) == false) {
				debug_check_page_map_status(new_vmr, head,
							head_idx, pmd, false);
				continue;
			}

			debug_check_page_map_status(new_vmr, head,
							head_idx, pmd, true);

			/* If prev_vmr is mapped, kernel copied the pte to
			 * new_vmr and increased the reference count of the
			 * page. Thus, we have to insert pmd to new_vmr.
			 */
			emp_lp_insert_pmd(emm, gpa->local_page, new_vmr->id, pmd);
			debug_lru_add_vmr_id_mark(gpa->local_page, new_vmr->id);
			debug_page_ref_dup_end(gpa->local_page);
			debug_page_ref_mark(new_vmr->id, gpa->local_page, 1); /* mark the kernel's increment on page count */

			/* NOTE: RSS is updated by kernel */
			emp_update_rss_add_kernel(new_vmr,
					__local_gpa_to_page_len(new_vmr, gpa),
					DEBUG_RSS_ADD_KERNEL_VMA_OPEN,
					gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
		}

		emp_unlock_block(head);
	}
}

int dup_vmdesc(struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr,
				const bool new_vmdesc, const bool dup_dir)
{
	dprintk("%s: new: %016lx prev: %016lx new_vmdesc: %d dup_dir: %d prev->descs: %016lx\n",
		__func__, (unsigned long) new_vmr, (unsigned long) prev_vmr,
		new_vmdesc, dup_dir, (unsigned long) prev_vmr->descs);

	if (new_vmdesc) {
		struct emp_vmdesc *desc;
		unsigned long gpa_dir_offset;
		new_vmr->descs = NULL;

		desc = alloc_vmdesc(prev_vmr->descs);
		if (unlikely(desc == NULL)) {
			printk("%s: ERROR: failed to allocate vm descriptor.\n",
					__func__);
			return -ENOMEM;
		}

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
	}

	__dup_vmdesc(new_vmr, prev_vmr, dup_dir);

	return 0;
}

void cow_init(struct emp_mm *emm) {
	emm->cops.handle_emp_cow_fault_hva = handle_emp_cow_fault_hva;
	spin_lock_init(&emm->dup_list_lock);
	emm->mrs.cow_remote_pages_cache = emp_kmem_cache_create("cow_remote_pages",
							sizeof(struct cow_remote_page),
							4, 0, NULL);
}

void cow_exit(struct emp_mm *emm) {
	emp_kmem_cache_destroy(emm->mrs.cow_remote_pages_cache);
}
#endif /* CONFIG_EMP_USER */
