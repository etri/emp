#include <linux/version.h>
#include <linux/mm.h>
#include <asm/pgalloc.h>
#include "config.h"
#include "vm.h"
#include "mm.h"
#include "hva.h"
#include "glue.h"
#include "reclaim.h"
#include "page_mgmt.h"
#include "block-flag.h"
#include "donor_mem_rw.h"
#include "debug.h"
#include "stat.h"
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
static void __pmd_populate(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	vmf->ptl = pmd_lock(vma->vm_mm, vmf->pmd);

	if (unlikely(!pmd_none(*vmf->pmd))) {
		spin_unlock(vmf->ptl);
		return;
	}

	if (vmf->prealloc_pte == NULL) {
		vmf->prealloc_pte = kernel_pte_alloc_one(vma->vm_mm,
							 vmf->address);
		smp_wmb();
	}

	mm_inc_nr_ptes(vma->vm_mm);
	pmd_populate(vma->vm_mm, vmf->pmd, vmf->prealloc_pte);
	spin_unlock(vmf->ptl);

	vmf->prealloc_pte = NULL;
}

static void emp_hpt_fetch_barrier(struct emp_mm *bvma, struct emp_gpa *head,
				  struct emp_gpa *demand, unsigned long pgoff,
				  struct emp_gpa *fs, struct emp_gpa *fe,
				  struct emp_gpa *prefetched_sb, bool fetch)
{
	struct emp_gpa *sb_head;
	struct vcpu_var *cpu = emp_this_cpu_ptr(bvma->pcpus);

	head->r_state = GPA_ACTIVE;
	set_gpa_flags_if_unset(head, GPA_HPT_MASK);
	clear_gpa_flags_if_set(head, GPA_REMOTE_MASK);


	// Assumption: only gpa descriptors in a block reside in a contiguous memory region.
	debug_BUG_ON((fe - fs) > num_subblock_in_block(demand));

	for_each_gpas(sb_head, head) {
		if (fetch && prefetched_sb) {
			if (sb_head == prefetched_sb)
				continue;
		}
		
		// ignore already mapped subblocks
		if (sb_head < fs || sb_head >= fe)
			continue;

		// we must put subblock for fetch_page
		// don't merge the put_subblock to get_subblock for pte_install
		if ((bvma->sops.wait_read_async)(bvma, cpu, sb_head)) {
			debug_page_ref_io_end(sb_head->local_page);
			emp_put_subblock(sb_head);
		}
	}
}

static int COMPILER_DEBUG
emp_install_hptes(struct emp_mm *bvma, struct emp_vmr *vmr, 
			struct emp_gpa *head, struct emp_gpa *demand,
			struct emp_gpa *fs, struct emp_gpa *fe,
			struct emp_gpa *prefetched_sb, bool fetch, 
			struct vm_fault *vmf, pmd_t *pmd, pmd_t orig_pmd)
{
	struct page *p;
	struct emp_gpa *sb_head;
	int ret = 0, r;
	unsigned int sb_order, sb_mask;
	bool csf_prefetching;
	struct vm_fault fault;

	set_vmf_pgoff(&fault, vmf->pgoff & gpa_block_mask(demand));
	set_vmf_address(&fault, (unsigned long)vmf->address & gpa_page_mask(demand));
	fault.flags = vmf->flags;
	
	// prefetched_sb != NULL => prefetch_hit == true => csf == false	
	// prefetched_sb == NULL => prefetch_hit == false => csf == GPA_PREFETCHED_MASK
	csf_prefetching = (prefetched_sb == NULL) && 
				is_gpa_flags_set(head, GPA_PREFETCHED_CSF_MASK);

	// Assumption: only gpa descriptors in a block reside in a contiguous memory region.
	debug_BUG_ON((fe - fs) > num_subblock_in_block(demand));

	/* TODO: csf_prefetching does not need a loop */
	for_each_gpas(sb_head, head) {
		sb_order = gpa_subblock_order(sb_head);
		sb_mask = gpa_subblock_mask(sb_head);
		p = sb_head->local_page->page;

		if (prefetched_sb) {
			/* prefetched demand sub-block/page is already serviced */
			if (sb_head == prefetched_sb)
				goto next;
		} else if (csf_prefetching) {
			/* only demand sub-block wait_read && mapping */
			if (demand != sb_head)
				goto next;
		}

		// ignore already mapped subblocks
		if (sb_head < fs || sb_head >= fe)
			goto next;

		fault.page = p;

		debug_page_ref_will_pte_beg(sb_head->local_page, gpa_subblock_size(sb_head));
		emp_get_subblock(sb_head, true);
		debug_page_ref_will_pte_end(sb_head->local_page, gpa_subblock_size(sb_head));

		r = pte_install(bvma, vmr->host_vma, &fault, sb_order, pmd, orig_pmd, head);

		debug_check_notnull_pointer(sb_head->local_page->w);

		// pmd is exclusive to w, so it must be used after pte_insetall
		emp_lp_insert_pmd(bvma, sb_head->local_page, vmr->id, pmd);
		debug_lru_add_vmr_id_mark(sb_head->local_page, vmr->id);

		if (demand == sb_head) {
			vmf->page = p + (vmf->pgoff & sb_mask);
			ret = r;
		}

next:
		/* for the next iteration */
		add_vmf_pgoff(&fault, gpa_subblock_size(sb_head));
		add_vmf_address(&fault, gpa_subblock_size(sb_head) << PAGE_SHIFT);
	}

	if (head->local_page->vmr_id != vmr->id)
		emp_update_rss_add_force(vmr,
				__local_block_to_page_len(vmr, head),
				DEBUG_RSS_ADD_INSTALL_HPTES,
				head, DEBUG_UPDATE_RSS_BLOCK);

	debug_clear_and_map_pages(head);

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
			struct emp_gpa *demand, unsigned long demand_idx,
			struct emp_gpa *fs, struct emp_gpa *fe,
			bool fetch, struct vm_fault *vmf, bool prefetch_hit)
{
	int ret;
	pmd_t *pmd, orig_pmd;
	bool demand_check;
	unsigned int sb_order = gpa_subblock_order(head);
	struct emp_gpa *prefetched_sb = prefetch_hit ?
			(head + (head->local_page->demand_offset >> sb_order)) :
			NULL;

	pmd = get_pmd(vmr->host_mm, (unsigned long)vmf->address, &pmd);

	orig_pmd = *pmd;
	if (pmd_none(*pmd))
		__pmd_populate(vmr->host_vma, vmf);

	demand_check = prefetch_hit && (prefetched_sb == demand);
	emp_hpt_fetch_barrier(emm, head, demand, demand_idx, fs, fe,
				prefetched_sb, fetch);
	
	ret = emp_install_hptes(emm, vmr, head, demand, fs, fe, prefetched_sb,
						fetch, vmf, pmd, orig_pmd);

	if (!demand_check && (ret != VM_FAULT_NOPAGE)) {
		printk(KERN_ERR "pmd_install does not succeed. "
				"pmd: %lx, orig_pmd: %lx\n",
				pmd_val(*pmd), pmd_val(orig_pmd));
		wait_event_interruptible_timeout(tmp_wq, 0, 15*HZ);
	}

	return ret;
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
	struct vm_area_struct *vma = vmf->vma;
	struct emp_mm *emm;
	struct emp_vmr *vmr;

	pgoff_t demand_off, orig_pgoff;
	struct emp_gpa *demand, *head;
	struct emp_gpa *fs, *fe; // fetch_start, fetch_end
	unsigned long head_idx;
	struct vcpu_var *cpu;
	bool fetch = false;
	unsigned int sb_order, demand_sb_off;
	int rss_count;
#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
	s64 __num_emp_hva_fault;
#endif

	vmr = __get_emp_vmr(vma);
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


	cpu = emp_this_cpu_ptr(emm->pcpus);

	sb_order = bvma_subblock_order(emm);
	demand_off = (vmf->address - vmr->descs->vm_base);
	demand_off >>= PAGE_SHIFT;
	orig_pgoff = vmf->pgoff;
	set_vmf_pgoff(vmf, demand_off);
	demand_sb_off = demand_off >> sb_order;
	demand = get_gpadesc(vmr, demand_sb_off);
	if (unlikely(!demand)) {
		errcode = -ENOMEM;
		ret = VM_FAULT_SIGBUS;
		goto _emp_page_fault_hva_out_unlocked;
	}

	emp_wait_for_writeback(emm, cpu, gpa_block_size(demand));

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


	/* specify the range of fetching */
	fs = head;
	fe = head + num_subblock_in_block(head);


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
		goto _emp_page_fault_hva_fetch_posted;
	}
#endif

	// all or nothing policy is assumed for a block
	if (head->r_state != GPA_INIT) {
		r = handle_local_fault(vmr, &head, demand, cpu, vmf, &ret);
		debug_progress(head, r);
		if (r != 0) {
			if (unlikely(r < 0)) {
				errcode = r;
				ret = VM_FAULT_SIGBUS;
			}
			goto _emp_page_fault_hva_out;
		}
		// head is updated if the gpa is stretched
	} else {
		/* if the gpa->local_page is NULL, alloc a page for local cache
		 * and fetch the data from remote(or local) donor.
		 */
		r = handle_remote_fault(vmr, &head, head_idx, demand,
					demand_off, cpu, false);
		debug_progress(head, r);
		if (likely(r >= 0)) {
			fetch = r > 0 ? true : false;
		} else {
			clear_in_flight_fetching_block(vmr, cpu, head);
			errcode = r;
			ret = VM_FAULT_SIGBUS;
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

		debug_BUG_ON(PageLocked(head->local_page->page));
		emp_lock_subblock(head);
	} else 
#endif
	{
		ret = emp_page_fault_hptes_map(emm, vmr, head, demand,
						demand_sb_off - head_idx,
						fs, fe, fetch, vmf, false);
		debug_progress(head, ret);
	}

	set_gpa_flags_if_unset(head, GPA_DIRTY_MASK);

	debug___emp_page_fault_hva2(head);

	rss_count = gpa_block_size(head);

_emp_page_fault_hva_out:
	emp_unlock_block(head);
_emp_page_fault_hva_out_unlocked:

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
	return ret;
}
