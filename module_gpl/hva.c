#include <linux/version.h>
#include <linux/mm.h>
#include <asm/pgalloc.h>
#include "config.h"
#include "vm.h"
#include "hva.h"
#include "glue.h"
#include "reclaim.h"
#include "page_mgmt.h"
#include "block-flag.h"
#include "donor_mem_rw.h"
#include "debug.h"
#include "stat.h"
#ifdef CONFIG_EMP_USER
#include "cow.h"
#endif
#include "pcalloc.h"

extern struct emp_mm **emp_mm_arr;
extern unsigned long emp_mm_arr_len;
DECLARE_WAIT_QUEUE_HEAD(tmp_wq);

#define vmf_write_fault(vmf) (((vmf)->flags & FAULT_FLAG_WRITE) ? true : false)

/**
 * get_pmd - Get a pmd entry for the fault address
 * @param mm mm structure
 * @param address fault address
 * @param pmd pmd entry
 *
 * @return pmd entry
 */
pmd_t *get_pmd(struct mm_struct *mm, unsigned long address, pmd_t **pmd)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *orig_pmd;

	pgd = pgd_offset(mm, address);
	p4d = p4d_offset(pgd, address);
	pud = pud_offset(p4d, address);
	*pmd = pmd_offset(pud, address);

	orig_pmd = *pmd;
	if (!pmd_none(*orig_pmd))
		return orig_pmd;

	spin_lock(&mm->page_table_lock);
	orig_pmd = *pmd;
	spin_unlock(&mm->page_table_lock);
	return orig_pmd;
}

/**
 * __pmd_populate - Populate pmd entry according to the fault address
 * @param vma virtual memory info
 * @param vmf fault address info
 */
static void __pmd_populate(struct mm_struct *mm, struct vm_fault *vmf)
{
	vmf->ptl = pmd_lock(mm, vmf->pmd);

	if (unlikely(!pmd_none(*vmf->pmd))) {
		spin_unlock(vmf->ptl);
		return;
	}

	if (vmf->prealloc_pte == NULL) {
		vmf->prealloc_pte = kernel_pte_alloc_one(mm, vmf->address);
		smp_wmb();
	}

	mm_inc_nr_ptes(mm);
	pmd_populate(mm, vmf->pmd, vmf->prealloc_pte);
	spin_unlock(vmf->ptl);

	vmf->prealloc_pte = NULL;
}

static void emp_hpt_fetch_barrier(struct emp_mm *bvma, struct emp_vmr *vmr,
		struct emp_gpa *head, struct emp_gpa *demand, unsigned long pgoff,
		struct emp_gpa *prefetched_gpa, bool fetch)
{
	struct emp_gpa *gpa;
	struct vcpu_var *cpu = emp_this_cpu_ptr(bvma->pcpus);
#ifdef CONFIG_EMP_BLOCK
	bool csf = bvma_csf_enabled(bvma);
	bool cpf = bvma_cpf_enabled(bvma);
#endif

	head->r_state = GPA_ACTIVE;
	set_gpa_flags_if_unset(head, GPA_HPT_MASK);
	clear_gpa_flags_if_set(head, GPA_REMOTE_MASK);

#ifdef CONFIG_EMP_BLOCK
	if (csf && fetch && !prefetched_gpa
			& !is_gpa_flags_set(head, GPA_PREFETCH_ONCE_MASK)) {
		if (cpf) {
			/* waiting only for demand page */
			int fallback;
			unsigned int sb_off = pgoff & gpa_subblock_mask(head);
			fallback = bvma->sops.wait_read_async_demand_page(bvma,
							cpu, demand, sb_off);
			/* processed only a page, not all */
			head->local_page->demand_offset =
						gpa_block_offset(head, pgoff);
			if (fallback > 1)
				set_gpa_flags_if_unset(head, GPA_PREFETCHED_CSF_MASK);
			else
				set_gpa_flags_if_unset(head, GPA_PREFETCHED_CPF_MASK);
			set_gpa_flags_if_unset(head, GPA_PREFETCH_ONCE_MASK);
		} else {
			/* waiting only for demand subblock */
			if ((bvma->sops.wait_read_async)(bvma, cpu, demand))
				clear_gpa_flags_if_set(demand, GPA_REMOTE_MASK);

			/* processed only a sub-block, not all */
			if (gpa_subblock_order(head) != gpa_block_order(head)) {
				head->local_page->demand_offset =
						gpa_block_offset(head, pgoff);
				set_gpa_flags_if_unset(head, GPA_PREFETCHED_CSF_MASK);
				set_gpa_flags_if_unset(head, GPA_PREFETCH_ONCE_MASK);
			}
		}
		return;
	}
#endif

	if (is_gpa_flags_set(head, GPA_PREFETCHED_CPF_MASK))
		prefetched_gpa = NULL;

	for_each_gpas(gpa, head) {
		if (fetch && gpa == prefetched_gpa)
			continue;

		// ignore already mapped subblocks
		if (emp_lp_lookup_vmr_id(gpa, vmr->id))
			continue;

		// we must put subblock for fetch_page
		// don't merge the put_subblock to get_subblock for pte_install
		if ((bvma->sops.wait_read_async)(bvma, cpu, gpa))
			clear_gpa_flags_if_set(demand, GPA_REMOTE_MASK);
	}
}

/**
 * pte_install - Install page table entries in the host page table
 * @param vma vm area where mapping(s) wiil be installed
 * @param pmd page middle directory (upper entry of pte)
 * @param page head page struct of subblock
 * @param haddr fault address
 & @param page_len number of pages of the fault subblock
 * @param is_write is it write fault?
 *
 * @retval VM_FAULT_NOPAGE(256): Success
 * @retval 0: Error
 */
int COMPILER_DEBUG
pte_install(struct vm_area_struct *vma, pmd_t *pmd, struct page *page,
		unsigned long haddr, unsigned int page_len, const bool is_write)
{
	pte_t pte_entry;
	pte_t *_pte, *pte;
	struct page *_page;
	int i;

	debug_pte_install(page, page_len);

	pte = pte_offset_map(pmd, haddr);
	// now, empty pte is guaranteed
	for (i = 0, _pte = pte, _page = page;
		i < page_len; i++, _pte++, _page++, haddr += PAGE_SIZE) {
		pte_entry = *_pte;
		if (unlikely(pte_val(pte_entry))) {
			/* CPF may have mapped this page */
			if (pte_pfn(pte_entry) == page_to_pfn(_page))
				continue;
			else {
#ifdef CONFIG_EMP_DEBUG
				int idx = _pte - pte;
				struct emp_vmr *vmr = __get_emp_vmr(vma);
				if (vmr->magic != EMP_VMR_MAGIC_VALUE
						|| vmr->id >= EMP_VMRS_MAX
						|| vmr->emm->vmrs[vmr->id] != vmr)
					vmr = NULL;
				printk(KERN_ERR "%s ERROR: already occupied. "
					"addr: %016lx idx: %d pte: %016lx pfn: %lx "
					"page: %016lx pfn: %lx flag: %016lx "
					"page[%d]: %016lx pfn: %lx flag: %016lx "
					"vm_start: %016lx vm_end: %016lx "
					"vm_flag: %016lx vm_base: %016lx\n",
					__func__, haddr, idx,
					pte_val(pte_entry), pte_pfn(pte_entry),
					(unsigned long) page, page_to_pfn(page),
					page->flags,
					idx, (unsigned long) (page + idx),
					page_to_pfn(page + idx), (page + idx)->flags,
					vma->vm_start, vma->vm_end,
					vma->vm_flags,
					vmr ? vmr->descs->vm_base : 0);
#endif
				BUG();
			}
		}
		/*
		   following mk_pte could be a problem without compiler optimization
		   with turning off the optimization of gcc (GCC) 4.4.7 20120313 (Red Hat 4.4.7-4)
		   there was a case that variable assignment of a mk_pte did not work correctly
		 */
		flush_icache_page(vma, _page);
		pte_entry = mk_pte(_page, vma->vm_page_prot);
		if (is_write)
			pte_entry = maybe_mkwrite(pte_mkdirty(pte_entry), vma);
		pte_entry = pte_mkold(pte_entry);
		kernel_page_add_file_rmap(_page, vma, false);
		update_mmu_cache(vma, haddr, _pte);
		set_pte_at(vma->vm_mm, haddr, _pte, pte_entry);
	}
	pte_unmap(pte);

	if (is_write)
		emp_set_page_dirty(page, page_len);

	return VM_FAULT_NOPAGE;
}

/* @retval VM_FAULT_NOPAGE(256): Success
 * @retval 0: Error
 */
static inline int COMPILER_DEBUG
__emp_install_hptes(struct emp_vmr *vmr, struct emp_gpa *gpa,
			unsigned long hva, unsigned int page_len,
			pmd_t *pmd, bool is_write)
{
	struct local_page *lp = gpa->local_page;
	struct page *page = lp->page;
	int ret;

	debug_page_ref_will_pte_beg(lp, page_len);
	__emp_get_pages_map(vmr, gpa, page_len);
	debug_page_ref_will_pte_end(lp, page_len);

	ret = pte_install(vmr->host_vma, pmd, page, hva, page_len, is_write);

	debug_check_notnull_pointer(lp->w);

	// pmd is exclusive to w, so it must be used after pte_insetall
	emp_lp_insert_pmd(vmr->emm, lp, vmr->id, pmd);
	debug_lru_add_vmr_id_mark(lp, vmr->id);

	if (lp->vmr_id != vmr->id)
		emp_update_rss_add(vmr, page_len,
				DEBUG_RSS_ADD_INSTALL_HPTES,
				head, DEBUG_UPDATE_RSS_BLOCK);

	return ret;
}

/* @retval VM_FAULT_NOPAGE(256): Success
 * @retval 0: Error
 */
static int COMPILER_DEBUG emp_install_hptes(struct emp_mm *bvma,
				struct emp_vmr *vmr, struct emp_gpa *head,
				struct emp_gpa *demand, pmd_t *pmd,
				bool prefetch_hit, bool is_write)
{
	struct emp_gpa *gpa;
	int ret = VM_FAULT_NOPAGE;
	unsigned long hva;
	unsigned int page_len;
	bool csf_prefetching, cpf_prefetching;
	struct vm_area_struct *vma = vmr->host_vma;
	spinlock_t *ptl;

	// ptl is spinlock of pmd page
	ptl = pte_lockptr(vma->vm_mm, pmd);

	// prefetch_hit == true => csf == false
	// prefetch_hit == false => csf == GPA_PREFETCHED_MASK
	if (prefetch_hit) {
		cpf_prefetching = false;
		csf_prefetching = false;
	} else {
		cpf_prefetching = is_gpa_flags_set(head, GPA_PREFETCHED_CPF_MASK);
		csf_prefetching = is_gpa_flags_set(head, GPA_PREFETCHED_CSF_MASK);
	}

	if (cpf_prefetching) {
		struct page *page;
		int sb_offset;
		struct vcpu_var *cpu = emp_this_cpu_ptr(bvma->pcpus);
		if (bvma_transition_csf(bvma) &&
				(bvma->sops.try_wait_read_async(bvma, cpu, demand))) {
			clear_gpa_flags_if_set(demand, GPA_REMOTE_MASK);
			clear_gpa_flags_if_set(head, GPA_PREFETCHED_CPF_MASK);
			set_gpa_flags_if_unset(head, GPA_PREFETCHED_CSF_MASK);
#ifdef CONFIG_EMP_STAT
			bvma->stat.cpf_to_csf_transition++;
#endif
			cpf_prefetching = false;
			csf_prefetching = true;
			// fall-through
		} else {
			debug_assert(demand);
			debug_assert(!emp_lp_lookup_vmr_id(demand, vmr->id));
			____local_gpa_to_hva_and_len(vmr, demand, hva, page_len);
			sb_offset = head->local_page->demand_offset
						& gpa_subblock_mask(demand);
			hva += PAGE_SIZE * sb_offset;
			page = demand->local_page->page + sb_offset;
			spin_lock(ptl);
			ret = pte_install(vmr->host_vma, pmd, page, hva, 1, is_write);
			spin_unlock(ptl);
			return ret;
		}
	}

	if (csf_prefetching) {
		// if csf_prefetching is true, demand should exist.
		debug_assert(demand);
		debug_assert(!emp_lp_lookup_vmr_id(demand, vmr->id));
		____local_gpa_to_hva_and_len(vmr, demand, hva, page_len);
		spin_lock(ptl);
		ret = __emp_install_hptes(vmr, demand, hva, page_len,
							pmd, is_write);
		spin_unlock(ptl);
		emp_update_rss_cached(vmr);
		return ret;
	}

	____local_gpa_to_hva_and_len(vmr, head, hva, page_len);

	spin_lock(ptl);
	for_each_gpas(gpa, head) {
		// ignore already mapped subblocks
		if (emp_lp_lookup_vmr_id(gpa, vmr->id))
			goto next;

		ret &= __emp_install_hptes(vmr, gpa, hva, page_len, pmd, is_write);

next:
		/* for the next iteration */
		hva += PAGE_SIZE << gpa_subblock_order(gpa);
		// we do not update page_len since partial map block consists
		// of a single subblock
	}
	spin_unlock(ptl);

	emp_update_rss_cached(vmr);

	debug_clear_and_map_pages(bvma, head);

	return ret;
}

/**
 * emp_page_fault_hptes_map - Mapping a block to the page table entry after handling a fault
 * @param emm emm data structure
 * @param head head of the block
 * @param demand demand page
 * @param fs first gpa for fetching (Fetch Start)
 * @param fe last gpa for fetching (Fetch End)
 * @param fetch should be fetch from donor
 * @param vmf fault address info
 * @param prefetch_hit already fetched page from previous fault
 *
 * @retval VM_FAULT_NOPAGE(256): Success
 * @retval 0: Error
 */
int COMPILER_DEBUG
emp_page_fault_hptes_map(struct emp_mm *emm, struct emp_vmr *vmr,
			struct emp_gpa *head,
			struct emp_gpa *demand, unsigned long demand_off,
			bool fetch, struct vm_fault *vmf, bool prefetch_hit)
{
	int ret;
	pmd_t *pmd;
	bool demand_check;
	bool is_write = vmf->flags & FAULT_FLAG_WRITE;
	unsigned int sb_order = gpa_subblock_order(head);
	struct emp_gpa *prefetched_sb = prefetch_hit ?
			(head + (head->local_page->demand_offset >> sb_order)) :
			NULL;

	pmd = get_pmd(vmr->host_mm, (unsigned long)vmf->address, &pmd);

	if (pmd_none(*pmd))
		__pmd_populate(vmr->host_mm, vmf);

	demand_check = prefetch_hit && (prefetched_sb == demand);
	emp_hpt_fetch_barrier(emm, vmr, head, demand, demand_off,
				prefetched_sb, fetch);

	if (prefetch_hit)
		sync_hpt_map_in_block(emm, head, is_write);
	
#ifdef CONFIG_EMP_EXT
	if (emp_ext.prepare_install_hptes)
		emp_ext.prepare_install_hptes(emm, vmr, head, demand,
							prefetch_hit, is_write);
#endif	
	ret = emp_install_hptes(emm, vmr, head, demand, pmd,
							prefetch_hit, is_write);
	vmf->page = demand->local_page->page
			+ (vmf->pgoff & gpa_subblock_mask(demand));

	if (!demand_check && (ret != VM_FAULT_NOPAGE)) {
		printk(KERN_ERR "pmd_install does not succeed. pmd: %lx\n",
					pmd_val(*pmd));
		// XXX: what's the role of the following line?
		wait_event_interruptible_timeout(tmp_wq, 0, 15*HZ);
	}

	return ret;
}

static inline void __sync_hpt_map_in_block(struct emp_mm *emm,
		struct emp_gpa *head, struct emp_gpa *gpa, const bool is_write)
{
	struct local_page *lp_head, *lp_gpa;
	struct mapped_pmd *p_head, *p_gpa;
	struct emp_vmr *vmr;

	debug_assert(head->local_page);
	debug_assert(gpa->local_page);

	lp_head = head->local_page;
	lp_gpa = gpa->local_page;

	if (emp_lp_count_pmd(lp_head) == 0 && emp_lp_count_pmd(lp_gpa) == 0)
		return;

	if (emp_lp_count_pmd(lp_head) == 1 && emp_lp_count_pmd(lp_gpa) == 1
			&& lp_head->pmds.vmr_id == lp_gpa->pmds.vmr_id)
		return;

	p_head = emp_lp_first_mapped_pmd(lp_head);
	p_gpa = emp_lp_first_mapped_pmd(lp_gpa);
	debug_assert(p_head || p_gpa);

	/* TODO: handle the error from emp_install_hptes().
	 * NOTE: __pmd_populate() is not required since at least one
	 *       of the subblocks has the mapping.
	 */
	while (p_head && p_gpa) {
		if (p_head->vmr_id == p_gpa->vmr_id) {
			p_head = emp_lp_next_mapped_pmd(lp_head, p_head);
			p_gpa = emp_lp_next_mapped_pmd(lp_gpa, p_gpa);
		} else if (p_head->vmr_id < p_gpa->vmr_id) {
			vmr = emm->vmrs[p_head->vmr_id];
			emp_install_hptes(emm, vmr, head, NULL,
						p_head->pmd, true, is_write);
			p_head = emp_lp_next_mapped_pmd(lp_head, p_head);
		} else {
			vmr = emm->vmrs[p_gpa->vmr_id];
			emp_install_hptes(emm, vmr, head, NULL,
						p_gpa->pmd, true, is_write);
			p_gpa = emp_lp_next_mapped_pmd(lp_gpa, p_gpa);
		}
	}

	if (p_head == NULL && p_gpa == NULL)
		return;

	while (p_head) {
		vmr = emm->vmrs[p_head->vmr_id];
		emp_install_hptes(emm, vmr, head, NULL,
					p_head->pmd, true, is_write);
		p_head = emp_lp_next_mapped_pmd(lp_head, p_head);
	}

	while (p_gpa) {
		vmr = emm->vmrs[p_gpa->vmr_id];
		emp_install_hptes(emm, vmr, head, NULL,
					p_gpa->pmd, true, is_write);
		p_gpa = emp_lp_next_mapped_pmd(lp_gpa, p_gpa);
	}
}

void sync_hpt_map_in_block(struct emp_mm *emm, struct emp_gpa *head,
							const bool is_write)
{
	struct emp_gpa *end = head + gpa_desc_size(head);
	struct emp_gpa *gpa;

	for (gpa = head + 1; gpa < end; gpa++) {
		__sync_hpt_map_in_block(emm, head, gpa, is_write);
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
		if (gpa->local_page->vmr_id != head->local_page->vmr_id)
			debug_lru_set_vmr_id_mark(gpa->local_page,
						head->local_page->vmr_id);
#endif
		gpa->local_page->vmr_id = head->local_page->vmr_id;
	}
}

/**
 * __emp_page_fault_hva - (*Entry Point*) Host page fault handling function
 * @param vma virtual memory address space structure
 * @param vmf faulted address info
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * The function is used in place of the existing page fault handling function
 * in Linux kernl
 */
vm_fault_t emp_page_fault_hva(struct vm_fault *vmf)
{
	int ret = 0, r, errcode = 0;
	struct emp_mm *emm;
	struct emp_vmr *vmr;

	pgoff_t demand_off, orig_pgoff;
	struct emp_gpa *demand, *head;
	struct emp_gpa *fs, *fe; // fetch_start, fetch_end
	unsigned long head_idx;
	struct vcpu_var *cpu;
	bool fetch = false;
#ifdef CONFIG_EMP_EXT
	bool skip_fetch = true;
	u64 ts_start;
	int ts_type;
#endif
	unsigned int sb_order, demand_sb_off;
	int rss_count;
#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
	s64 __num_emp_hva_fault;
#endif

	vmr = __get_emp_vmr(vmf->vma);
	if (!vmr || ((emm = vmr->emm) == NULL)) {
		printk(KERN_ERR "failed to find a proper vma.\n");
		return VM_FAULT_SIGSEGV;
	}

#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
	__num_emp_hva_fault = atomic64_inc_return(&num_emp_hva_fault);
	if ((__num_emp_hva_fault == 1) ||
			(__num_emp_hva_fault % CONFIG_EMP_SHOW_FAULT_PROGRESS_PERIOD == 0))
		printk(KERN_ERR "[EMP_PROGRESS] %s num_hva_fault: %lld "
				"emm: %d vmr: %d addr: 0x%lx\n",
				__func__, __num_emp_hva_fault,
				emm->id, vmr->id, vmf->address);
#endif

#ifdef CONFIG_EMP_EXT
	// record start of gpa handling
	ts_start = get_ts_in_ns();
	ts_type = EMP_OP_LOCAL;
#endif

	cpu = emp_this_cpu_ptr(emm->pcpus);
#ifdef CONFIG_EMP_STAT
	inc_vma_fault(cpu);
#endif
	emp_pf_history_beg(cpu, vmr->id, vmf->address);
	emp_pf_history_add(cpu, hva_or_gpa, 0);

	sb_order = bvma_subblock_order(emm);
	demand_off = (vmf->address - vmr->descs->vm_base);
	demand_off >>= PAGE_SHIFT;
	orig_pgoff = vmf->pgoff;
	set_vmf_pgoff(vmf, demand_off);
	demand_sb_off = demand_off >> sb_order;
	demand = get_gpadesc(vmr, demand_sb_off);
	emp_pf_history_add(cpu, gpa_offset, demand_sb_off);
	if (unlikely(!demand)) {
		errcode = -ENOMEM;
		ret = VM_FAULT_SIGBUS;
		emp_pf_history_add(cpu, goto_code, 1);
		goto _emp_page_fault_hva_out_unlocked;
	}

#ifdef CONFIG_EMP_EXT
	if (emp_ext.prepare_map_hva)
		skip_fetch = emp_ext.prepare_map_hva(emm, vmr, vmf);
	
	if (!skip_fetch)
	/* if the gpa->local_page is NULL,
	 * pages for local cache should be allocated and page contents
	 * should be fetched from remote(or local) donor. */
		emp_wait_for_writeback(emm, cpu, gpa_block_size(demand));
#else
	emp_wait_for_writeback(emm, cpu, gpa_block_size(demand));
#endif

	/*
	 * file-backed page does not support thp currently.
	 * so no page can be assumed if the pmd is huge page.
	 */
	head = emp_lock_block(vmr, &demand, demand_sb_off);
	debug_BUG_ON(!head); // we already have @demand
	debug_progress(head, (((u64) vmf->flags) << 32)
				| ((vmf->flags & FAULT_FLAG_ALLOW_RETRY) ? 0x1000 : 0)
				| ((vmf->flags & FAULT_FLAG_TRIED) ? 0x100 : 0)
				| ((vmf->flags & FAULT_FLAG_MKWRITE) ? 0x10 : 0)
				| ((vmf->flags & FAULT_FLAG_WRITE) ? 0x1 : 0)
				);
	/* Assume that gpa descriptors in a block reside on a contiguous memory */
	head_idx = demand_sb_off - (demand - head);
	emp_pf_history_add(cpu, gpa_flag_beg, __get_gpa_flags(demand));
	emp_pf_history_add(cpu, head_flag_beg, __get_gpa_flags(head));
	emp_pf_history_add(cpu, head_state_beg, head->r_state);
	emp_pf_history_add(cpu, head_offset, head_idx);
	emp_pf_history_add(cpu, is_write, vmf_write_fault(vmf));

#ifdef CONFIG_EMP_USER
	if (vmf_write_fault(vmf)) {
		r = emm->cops.handle_emp_cow_fault_hva(emm, vmr, head, head_idx, vmf);
		emp_pf_history_add(cpu, cow_ret, r);
		debug_progress(head, r);
		if (r != 0) {
			/* If CoW fault is detected or error occurs,
			 * gpa descriptors may be changed. */
			demand = get_gpadesc(vmr, demand_sb_off);
			if (unlikely(!demand)) {
				errcode = -ENOMEM;
				ret = VM_FAULT_SIGBUS;
				debug_progress(head, 0);
				emp_pf_history_add(cpu, goto_code, 2);
				goto _emp_page_fault_hva_out_unlocked;
			}
			head = emp_get_block_head(demand);
			head_idx = demand_sb_off - (demand - head);
			debug_BUG_ON(!____emp_gpa_is_locked(head));
		}

		if (unlikely(r < 0)) { // error occurs.
			ret = VM_FAULT_SIGBUS;
			emp_pf_history_add(cpu, goto_code, 3);
			goto _emp_page_fault_hva_out;
		}
	}
#endif

	/* specify the range of fetching */
	fs = head;
	fe = head + num_subblock_in_block(head);

#ifdef CONFIG_EMP_BLOCK
	/* release (wait && map) all the prefetched sub-blocks in a block */
	if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK)) {
		debug___emp_page_fault_hva(head);

		if (is_gpa_flags_set(head, GPA_HPT_MASK)) {
			emp_page_fault_hptes_map(emm, vmr, head, demand,
						demand_off, true, vmf, true);
		}
#ifdef CONFIG_EMP_VM
		else if (is_gpa_flags_set(head, GPA_EPT_MASK)) {
			struct kvm_memory_slot *ms;
			u64 head_gpa = hva_to_gpa(emm, vmf->address, &ms) -
				((demand - head) << (sb_order + PAGE_SHIFT)) -
				((demand_off & gpa_subblock_mask(demand)) << PAGE_SHIFT);
			emp_page_fault_sptes_map(kvm_get_any_vcpu(emm->ekvm.kvm),
						 cpu, head, head_gpa, ms);
		}
#endif /* CONFIG_EMP_VM */

		clear_gpa_flags_if_set(head, GPA_PREFETCHED_MASK);
#ifdef CONFIG_EMP_STAT
		emm->stat.csf_fault++;
#endif /* CONFIG_EMP_STAT */
	}
#endif /* CONFIG_EMP_BLOCK */

#ifdef CONFIG_EMP_EXT
	if (emp_ext.early_handle_fault_hva) {
		if (emp_ext.early_handle_fault_hva(emm, vmr, vmf,
						demand, demand_sb_off)) {
			debug_progress(head, 0);
			emp_pf_history_add(cpu, goto_code, 4);
			goto _emp_page_fault_hva_out;
		}
	}
#endif
#ifdef CONFIG_EMP_IO
	// following must be cleared
	if (is_gpa_flags_set(head, GPA_IO_IP_MASK)) {
		printk(KERN_DEBUG "%s page is already in progress %lx "
				"ppid: %d cpid: %d",
				__func__, demand_off,
				demand->pid, current->pid);
		clear_gpa_flags_if_set(head, GPA_IO_IP_MASK);
		
		debug_BUG_ON(!PageLocked(head->local_page->page));
		emp_unlock_subblock(head);
		
		debug_progress(head, 0);
		emp_pf_history_add(cpu, goto_code, 5);
		goto _emp_page_fault_hva_fetch_posted;
	}
#endif

	// all or nothing policy is assumed for a block
	emp_pf_history_add(cpu, head_state_mid, head->r_state);
	if (head->r_state != GPA_INIT) {
#ifdef CONFIG_EMP_STAT
		inc_local_fault(cpu);
#endif
		r = handle_local_fault(vmr, &head, demand, cpu, vmf, &ret);
		debug_progress(head, r);
		emp_pf_history_add(cpu, local_fault_ret, r);
		emp_pf_history_add(cpu, remote_fault_ret, -1);
		if (r != 0) {
			if (unlikely(r < 0)) {
				errcode = r;
				ret = VM_FAULT_SIGBUS;
			}
			emp_pf_history_add(cpu, goto_code, 6);
			goto _emp_page_fault_hva_out;
		}
		// head is updated if the gpa is stretched
	} else {
		/* if the gpa->local_page is NULL, alloc a page for local cache
		 * and fetch the data from remote(or local) donor.
		 */
#ifdef CONFIG_EMP_EXT
		r = emp_ops.handle_remote_fault(vmr, &head, head_idx, demand,
						    demand_off, cpu, false);
		if (r > 0)
			ts_type = EMP_OP_REMOTE;
#else
		r = handle_remote_fault(vmr, &head, head_idx, demand,
					demand_off, cpu, false);
#endif
		emp_pf_history_add(cpu, local_fault_ret, -1);
		emp_pf_history_add(cpu, remote_fault_ret, r);
		debug_progress(head, r);
		if (likely(r >= 0)) {
			fetch = r > 0 ? true : false;
		} else {
			clear_in_flight_fetching_block(vmr, cpu, head);
			errcode = r;
			ret = VM_FAULT_SIGBUS;
			emp_pf_history_add(cpu, goto_code, 7);
			goto _emp_page_fault_hva_out;
		}
	}

#ifdef CONFIG_EMP_IO
	/* here, when page fault came from io thread for emulating virtio-blk,
	 * an insertion of page mapping to hva and unlocking are delayed
	 */
_emp_page_fault_hva_fetch_posted:
	if (clear_gpa_flags_if_set(head, GPA_IO_WRITE_MASK) && 
			fetch && demand->pid == current->pid) {
		printk("waiting completion of write page is delayed");
		vmf->page = (demand->local_page->page +
				(demand_off & gpa_subblock_mask(demand)));
		ret = VM_FAULT_RETRY;
		set_gpa_flags_if_unset(head, GPA_IO_IP_MASK);
		debug_progress(head, ret);
		emp_pf_history_add(cpu, install_pte_ret, 0xFFFFFFFF);

		debug_BUG_ON(PageLocked(head->local_page->page));
		emp_lock_subblock(head);
	} else 
#endif
	{
		ret = emp_page_fault_hptes_map(emm, vmr, head, demand,
						demand_off, fetch, vmf, false);
		emp_pf_history_add(cpu, install_pte_ret, ret);
		debug_progress(head, ret);
	}

#ifdef CONFIG_EMP_EXT
	// XXX we must set the block as dirty???
	if (!set_gpa_flags_if_unset(head, GPA_DIRTY_MASK)) {
		/* if the previous value is DIRTY, do not notify. */
		if (emp_ext.emp_set_block_dirty_notifier)
			emp_ext.emp_set_block_dirty_notifier(head);
	}
#else
	set_gpa_flags_if_unset(head, GPA_DIRTY_MASK);
#endif

	debug___emp_page_fault_hva2(emm, head);

#ifdef CONFIG_EMP_USER
	if (!is_gpa_flags_set(head, GPA_PARTIAL_MAP_MASK)) {
		rss_count = gpa_block_size(head);
	} else {
		int shm_count = emp_lp_count_pmd(head->local_page);
		rss_count = page_count(head->local_page->page) - 1;
		rss_count /= shm_count;
	}
#else
	rss_count = gpa_block_size(head);
#endif
	emp_pf_history_add(cpu, rss_count, rss_count);

_emp_page_fault_hva_out:
	emp_pf_history_add(cpu, gpa_flag_end, __get_gpa_flags(demand));
	emp_pf_history_add(cpu, head_flag_end, __get_gpa_flags(head));
	emp_pf_history_add(cpu, head_state_end, head->r_state);
	emp_unlock_block(head);
_emp_page_fault_hva_out_unlocked:

#ifdef CONFIG_EMP_EXT
	if (likely(ret == VM_FAULT_NOPAGE || ret == VM_FAULT_RETRY)
		&& emp_ext.finish_page_fault_notifier)
		emp_ext.finish_page_fault_notifier(emm, ts_start, ts_type, cpu);
#endif
	set_vmf_pgoff(vmf, orig_pgoff);
	if (unlikely(ret == VM_FAULT_SIGBUS))
		printk(KERN_ERR "%s returns SIGBUS. code: %d"
				" pgoff: %lx last_mr: %d local: %c remote: %llx fetch: %d\n",
				__func__, -errcode,
				demand_off,
				demand ? demand->last_mr_id : -1,
				(demand && demand->local_page)
					? 'V' : 'N',
				demand ? get_gpa_remote_page_val(demand) : -1,
				fetch ? 1 : 0);
	emp_pf_history_add(cpu, error_code, errcode);
	emp_pf_history_add(cpu, page_fault_ret, ret);
	emp_pf_history_end(cpu);
	return ret;
}
