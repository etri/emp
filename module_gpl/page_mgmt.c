#include <asm/page.h>
#include "vm.h"
#include "gpa.h"
#include "glue.h"
#include "reclaim.h"
#include "page_mgmt.h"
#include "donor_mem_rw.h"
#include "block-flag.h"
#include "kvm_mmu.h"
#include "kvm_emp.h"
#ifdef CONFIG_EMP_STAT
#include "stat.h"
#endif
#include "debug.h"
#include "paging.h"
#include "hva.h"
#include "pcalloc.h"

#ifdef CONFIG_EMP_VM
/* from kvm/mmu/mmu.c */
enum {
	RET_PF_RETRY = 0,
	RET_PF_EMULATE = 1,
	RET_PF_INVALID = 2,
};

static inline void *get_kvm_emp_mm(struct kvm *kvm) {
	return container_of(kvm, struct kvm_emp_container, kvm)->emp_mm;	
}
#endif

/**
 * _handle_writeback_fault - Handle the page fault which is going to writeback
 * @param vmr vmr data structure
 * @param head heads of faulted blocks
 * @param cpu working vcpu ID
 *
 * Handle page fault when the page is going to writeback to donor.
 */
static void _handle_writeback_fault(struct emp_vmr *vmr, struct emp_gpa *head,
				    struct vcpu_var *cpu)
{
	struct emp_gpa *g;
	struct emp_mm *emm = vmr->emm;
	struct work_request *head_wr = head->local_page ? head->local_page->w
							: NULL;
	int prev_vmr_id = head->local_page->vmr_id;

	debug_assert(head->local_page && head->local_page->w);

	debug_progress(head_wr, head);
	emm->sops.clear_writeback_block(emm, head, head_wr, cpu,
					true, false);
	// decrement of inactive_list.page_len is delayed
	sub_inactive_list_page_len(emm, head);

	for_each_gpas_reverse(g, head) {
		free_remote_page(emm, g, true);
		g->local_page->vmr_id = vmr->id;
		debug_lru_set_vmr_id_mark(g->local_page, vmr->id);
	}

	if (prev_vmr_id != vmr->id) {
		if (prev_vmr_id >= 0) {
			struct emp_vmr *prev_vmr = emm->vmrs[prev_vmr_id];
			emp_update_rss_sub_force(prev_vmr,
				__local_block_to_page_len(prev_vmr, head),
				DEBUG_RSS_SUB_WRITEBACK_PREV,
				head, DEBUG_UPDATE_RSS_BLOCK);
		}
		emp_update_rss_add_force(vmr, __local_block_to_page_len(vmr, head),
				DEBUG_RSS_ADD_WRITEBACK_CURR,
				head, DEBUG_UPDATE_RSS_BLOCK);
	}
}

/**
 * handle_writeback_fault - Handle the page fault which is going to writeback
 * @param vmr vmr data structure
 * @param head heads of faulted blocks
 * @param dma_head gpa for faulted addr
 * @param cpu working vcpu ID
 *
 * Handle page fault when the page is going to writeback to donor. \n
 * Update LRU list since the page is re-referenced
 */
int handle_writeback_fault(struct emp_vmr *vmr, struct emp_gpa **head,
			    struct emp_gpa *gpa, struct vcpu_var *cpu)
{
	_handle_writeback_fault(vmr, *head, cpu);

	return update_lru_lists_reref(vmr->emm, cpu, head, 1, gpa_block_size(*head));
}

/**
 * _handle_gpa_on_inactive_fault - Handle local inactive page fault
 * @param vmr vmr data structure
 * @param head heads of faulted blocks
 * @param cpu working vcpu ID
 *
 * Handle page fault when the page is in inactive list.
 */
/* TODO handle_gpa_* is too ambiguous function names */
static void
_handle_gpa_on_inactive_fault(struct emp_vmr *vmr, struct emp_gpa *head,
			      struct vcpu_var *cpu)
{
	int gpa_cpu;
	struct emp_list *list;
	struct emp_mm *emm = vmr->emm;
	struct slru *slru = &emm->ftm.inactive_list;
	int prev_vmr_id = head->local_page->vmr_id;

	gpa_cpu = get_local_page_cpu(head->local_page);
	list = get_list_ptr_inactive(emm, slru, gpa_cpu);
	emp_list_lock(list);
	emp_list_del(&head->local_page->lru_list, list);
	emp_list_unlock(list);

	if (prev_vmr_id != vmr->id) {
		struct emp_gpa *g;

		for_each_gpas(g, head) {
			g->local_page->vmr_id = vmr->id;
			debug_lru_set_vmr_id_mark(g->local_page, vmr->id);
		}
	}

	debug__handle_gpa_on_inactive_fault(head);

	head->r_state = GPA_INIT;

	sub_inactive_list_page_len(emm, head);

	if (prev_vmr_id != vmr->id) {
		if (prev_vmr_id >= 0) {
			struct emp_vmr *prev_vmr = emm->vmrs[prev_vmr_id];
			emp_update_rss_sub_force(prev_vmr,
				__local_block_to_page_len(prev_vmr, head),
				DEBUG_RSS_SUB_INACTIVE_PREV,
				head, DEBUG_UPDATE_RSS_BLOCK);
		}
		emp_update_rss_add_force(vmr,
				__local_block_to_page_len(vmr, head),
				DEBUG_RSS_ADD_INACTIVE_CURR,
				head, DEBUG_UPDATE_RSS_BLOCK);
	}
}

/**
 * handle_inactive_fault - Handle local inactive page fault
 * @param bvma bvma data structure
 * @param head heads of faulted blocks
 * @param dma_head gpa for faulted addr
 * @param cpu working vcpu ID
 *
 * Handle page fault when the page is in inactive list.
 * The handled page is promoted to active list
 */
void handle_inactive_fault(struct emp_vmr *vmr, struct emp_gpa **head,
			   struct emp_gpa *dma_head, struct vcpu_var *cpu)
{
	struct emp_gpa *phead = *head;

	_handle_gpa_on_inactive_fault(vmr, phead, cpu);

	add_gpas_to_active_list(vmr->emm, cpu, head, 1);
}

/**
 * handle_active_fault - Handle local active page fault
 * @param vmr vmr data structure
 * @param head heads of faulted blocks
 * @param dma gpa for faulted addr
 * @param vcpu working vcpu ID
 * @param vmf_ret return value after handling the fault
 *
 * @retval 0: gpa needs to be inserted to page table
 * @retval 1: gpa already handled so just return
 *
 * Handle page fault when the page is in active list.
 */
int handle_active_fault(struct emp_vmr *vmr, struct emp_gpa *head,
		        struct emp_gpa *dma, struct vcpu_var *cpu,
			struct vm_fault *vmf, int *vmf_ret)
{
	struct mapped_pmd *p, *pp;

	if (!IS_IOTHREAD_VCPU(cpu->id))
		return 0;

	// check concurrent fault by vcpu_thread
	// if then, we must map the page to HVA
	if (clear_gpa_flags_if_set(head, GPA_STALE_BLOCK_MASK))
		return 0;
#ifdef CONFIG_EMP_VM
	if (is_gpa_flags_same(head, GPA_nPT_MASK, GPA_EPT_MASK))
		return 0;
#endif

	if (emp_lp_lookup_pmd(head->local_page, vmr->id, &p, &pp)) {
		*vmf_ret = VM_FAULT_NOPAGE;
		return 1;
	} else
		return 0;
}

/**
 * handle_local_fault - Handle local page fault
 * @param bvma bvma data structure
 * @param head heads of faulted blocks
 * @param gpa gpa for faulted addr
 * @param vcpu working vcpu ID
 * @param vmf_ret return value after handling the fault
 *
 * @retval 0: gpa needs to be inserted to page table
 * @retval 1: gpa already handled so just return
 * @retval -1: error occurs
 */
int handle_local_fault(struct emp_vmr *vmr, struct emp_gpa **head,
		       struct emp_gpa *gpa, struct vcpu_var *cpu,
		       struct vm_fault *vmf, int *vmf_ret)
{
	int ret = 0;

	switch ((*head)->r_state) {
		case GPA_ACTIVE:
			ret = handle_active_fault(vmr, *head, gpa, cpu,
					vmf, vmf_ret);
			break;

		case GPA_INACTIVE:
			handle_inactive_fault(vmr, head, gpa, cpu);
			break;

		case GPA_WB:
			ret = handle_writeback_fault(vmr, head, gpa, cpu);
			/* inserting to page table is required */
			if (ret > 0)
				ret = 0;
			break;

		case GPA_FETCHING:
			printk(KERN_ERR "GPA_FETCHING should be a hidden state\n");
			BUG();

		default:
			printk(KERN_ERR "invalid head state: %d\n", 
					(*head)->r_state);
			BUG();
	}
	
	return ret;
}

/**
 * fetch_block - Fetch multiple pages in a block
 * @param bvma bvma data structure
 * @param head head of the faulted block
 * @param demand_offset offset of the demand page from a block
 * @param vcpu_id working vcpu ID
 * @param flag flag of the gpa
 *
 * @retval 1: Fetch success
 * @retval 0: Fetch failed
 * @retval -1: error
 *
 * It calls alloc_and_fetch_page function (block_size / page_size) times. \n
 * First, it fetches a demand page first with priority. \n
 * And then, it fetches the rest of pages in a block for prefetching
 */
/* fetch multiple pages in a block. it calls alloc_and_fetch_page function 
 * (block_size / page_size) times, the alloc_and_fetch_page funnction fetchs
 * multiple pages in a single local_pages 
 * emp block uses 4k local_page */
static int fetch_block(struct emp_mm *bvma, struct emp_vmr *vmr,
			struct emp_gpa *head, unsigned long head_idx,
			int demand_offset, struct vcpu_var *cpu,
			bool no_fetch, bool is_stale, bool io_read_mask)
{
	int ret, fip;
	unsigned int sb_order;
	int no_fetching_count = 0;
	struct emp_gpa *g, *demand;
	unsigned long offset;
	struct work_request *head_wr = NULL;

	WARN_ON_ONCE(head->block_order < bvma_subblock_order(bvma));

	sb_order = gpa_subblock_order(head);
	offset = demand_offset >> sb_order;
	demand = head + offset;

	
	// for the first touch on a block
	if (unlikely(!is_gpa_flags_set(head, GPA_TOUCHED_MASK))) {
		set_gpa_flags_if_unset(head, GPA_TOUCHED_MASK);
		no_fetch = true;
	}
	
	ret = bvma->sops.alloc_and_fetch_pages(vmr, demand, head_idx + offset,
						sb_order, demand_offset, cpu,
						NULL, NULL, 1, no_fetch,
						is_stale, io_read_mask);
	if (ret == 0)
		no_fetching_count++;
	else if (ret > 0)
		head_wr = demand->local_page->w;
	else /* error */
		return ret;
	fip = ret;

	// fetch the rest of pages in a block for prefetching
	for_each_gpas_index(g, offset, head) {
		if (g == demand)
			continue;

		ret = bvma->sops.alloc_and_fetch_pages(vmr, g, head_idx + offset,
					       sb_order, -1, cpu, head_wr, NULL,
					       bvma_csf_enabled(bvma)? 0: 1,
					       no_fetch, is_stale, io_read_mask);
		if (ret == 0)
			no_fetching_count++;
		else if (unlikely(ret < 0))
			return ret;
		else if (head_wr == NULL) /* && ret > 0 */ {
			head_wr = g->local_page->w;
			fip = 1;
		}
	}

	if (IS_IOTHREAD_VCPU(cpu->id) || fip == 0)
		head->r_state = GPA_ACTIVE;
	else
		head->r_state = GPA_FETCHING;

#ifdef CONFIG_EMP_STAT
	bvma->stat.stale_page_count += no_fetching_count;
#endif

	emp_update_rss_cached(vmr);

#ifdef CONFIG_EMP_BLOCKDEV
	if (bvma->mrs.blockdev_used && fip > 0)
		io_schedule();
#endif

	return fip;
}

/**
 * handle_remote_fault - handle remote page fault
 * @param vmr memory region which contain faulted address
 * @param head head of the faulted block
 * @param gpa gpa of the block
 * @param demand_off page offset of faulted address
 * @param vcpu working vcpu info
 * @param read_only_mapping read only flag
 *
 * @retval 1: fetching in progress
 * @retval 0: do not need fetching
 * @retval -1: error
 *
 * It calls $fetch_block, and decides increase or shrink the size of block
 */
int handle_remote_fault(struct emp_vmr *vmr, struct emp_gpa **head,
			unsigned long head_idx, struct emp_gpa *gpa,
			pgoff_t demand_off, struct vcpu_var *cpu,
			bool read_only_mapping)
{
	int fip, r;
	bool no_fetch = false, is_stale = false, io_read_mask = false;
	int block_size, block_order;
	struct emp_mm *bvma = vmr->emm;
	int demand_offset = gpa_block_offset(*head, demand_off);

	block_size = gpa_block_size(*head);
	block_order = gpa_block_order(*head);

#if defined(GPA_IO_READ_MASK)
	if (clear_gpa_flags_if_set(*head, GPA_IO_READ_MASK)) {
		no_fetch = true;
		io_read_mask = true;
	}
#endif
#ifdef CONFIG_EMP_OPT
	if (clear_gpa_flags_if_set(*head, GPA_STALE_BLOCK_MASK)) {
		no_fetch = true;
		is_stale = true;
	}
#endif

	// fetching a page on demand
	fip = fetch_block(bvma, vmr, *head, head_idx, demand_offset, cpu, no_fetch,
				is_stale, io_read_mask);
	debug_progress(*head, fip);
	if (unlikely(fip < 0))
		return fip;

	debug_fetch_block(*head, fip);

#ifdef CONFIG_EMP_STAT
	/* update stats - increase fetch num counters for the block order */
	if (fip) {
		inc_remote_fault(cpu);
	} else {
		inc_local_fault(cpu);
		if (!IS_IOTHREAD_VCPU(cpu->id)) {
			/* update gpa info */
			(*head)->r_state = GPA_ACTIVE;
		}

#if defined(GPA_IO_READ_MASK)
		if (no_fetch)
			bvma->stat.stale_page_count += block_size;
#endif
#ifdef CONFIG_EMP_OPT
		if (is_stale) {
			struct emp_gpa *g;
			// make block state broken, however no need to mark dirty 
			// because a fault generated by clear_page is write fault
			for_each_gpas(g, *head) {
				// need to free remote pages for this block
				clear_gpa_flags_if_set(g, GPA_REMOTE_MASK);
				free_remote_page(bvma, g, true);
			}
		}
#endif
	}
#else /* !CONFIG_EMP_STAT */
	if (!fip && !IS_IOTHREAD_VCPU(cpu->id)) {
		/* update gpa info */
		(*head)->r_state = GPA_ACTIVE;
	}
#endif

	/* update_lru_lists() selects and evicts a victim by updating lru lists.
	 * This function requires the size of a block to determine the block_size
	 * on cache systems. */
	r = update_lru_lists(bvma, cpu, head, 1, block_size);
	debug_progress(*head, r);
	if (unlikely(r < 0))
		return r;

	return fip;
}

#ifdef CONFIG_EMP_VM
#ifdef CONFIG_EMP_OPT
int emp_mark_free_pages(struct kvm *kvm, gfn_t gfn, long num_pages)
{
	struct kvm_memory_slot *s;
	unsigned long hva, req_offset;
	long reclaimed = 0;
	struct emp_vmr *vmr;
	struct emp_gpa *head, *req;
	gfn_t gfn_end = gfn + num_pages;
	struct emp_mm *emm = get_kvm_emp_mm(kvm);

	while (gfn < gfn_end) {
		s = gfn_to_memslot(kvm, gfn);
		if (!s || s->flags & KVM_MEMSLOT_INVALID) {
			printk(KERN_ERR "[%s] Failed to get memslot. "
					"gfn: %llx num_pages: %ld "
					"reclaimed: %ld memslot: %p\n",
					__func__, gfn, num_pages, reclaimed, s);
			break; /* failed */
		}

		hva = __gfn_to_hva_memslot(s, gfn);
		if ((vmr = emp_vmr_lookup_hva(emm, hva)) == NULL) {
			printk(KERN_ERR "[%s] Failed to get hva. "
					"gfn: %llx num_pages: %ld "
					"reclaimed: %ld memslot: %p\n",
					__func__, gfn, num_pages, reclaimed, s);
			break; /* failed */
		}

		req_offset = GPN_OFFSET(emm, HVA_TO_GPN(emm, vmr, hva)) 
						>> bvma_subblock_order(emm);
		if (req_offset >= vmr->descs->gpa_len) {
			printk(KERN_ERR "[%s] Failed to get bgpa. "
					"gfn: %llx num_pages: %ld reclaimed: %ld "
					"memslot: %p hva: 0x%lx req_offset: 0x%lx "
					"gpas_len: 0x%lx\n",
					__func__, gfn, num_pages, reclaimed, s,
					hva, req_offset, vmr->descs->gpa_len);
			break; /* failed */
		}

		req = raw_get_gpadesc(vmr, req_offset);
		if (!req) {
			/* nothing to do with untouched subblock */
			struct gpadesc_region *region;
			region = get_gpadesc_region(vmr->descs, req_offset);
			gfn = gfn - (gfn & ((1UL << region->block_order) - 1))
					+ (1UL << region->block_order);
			continue;
		}
		head = emp_lock_block(vmr, &req, req_offset);
		debug_BUG_ON(!head); // we already have @req

		if (gfn + gpa_block_size(head) > gfn_end) {
			printk(KERN_ERR "[%s] Failed to reclaim the last block. "
					"gfn: %llx num_pages: %ld reclaimed: %ld "
					"memslot: %p hva: 0x%lx gpa_block_size: %ld\n",
					__func__, gfn, num_pages, reclaimed, s,
					hva, gpa_block_size(head));
			emp_unlock_block(head);
			break;
		}

		if (INIT_BLOCK(head)) {
			set_gpa_flags_if_unset(head, GPA_STALE_BLOCK_MASK);
			
			reclaimed += gpa_block_size(head);
		} else if (INACTIVE_BLOCK(head)) {
			set_gpa_flags_if_unset(head, GPA_STALE_BLOCK_MASK);
			clear_gpa_flags_if_set(head, GPA_DIRTY_MASK);

			reclaimed += gpa_block_size(head);
		} else if (ACTIVE_BLOCK(head)) {
			bool tlb_flush_force = false;

			set_gpa_flags_if_unset(head, GPA_STALE_BLOCK_MASK);
			clear_gpa_flags_if_set(head, GPA_DIRTY_MASK);
			
			emm->vops.unmap_gpas(emm, head, &tlb_flush_force);

			reclaimed += gpa_block_size(head);
		} else if (WB_BLOCK(head)) { 
			set_gpa_flags_if_unset(head, GPA_STALE_BLOCK_MASK);

			reclaimed += gpa_block_size(head);
		} else { // FETCHING
			/* cannot reclaim the pages in the fetching state. */
		}

		gfn += gpa_block_size(head);
		emp_unlock_block(head);
	}

	return reclaimed;
}
#endif /* CONFIG_EMP_OPT */

static inline void shadow_page_walk_begin(struct kvm_vcpu *vcpu)
	__attribute__((optimize("-O2")));
static inline void shadow_page_walk_begin(struct kvm_vcpu *vcpu)
{
	kvm_emp_walk_shadow_page_lockless_begin(vcpu);
}

static inline void shadow_page_walk_end(struct kvm_vcpu *vcpu)
	__attribute__((optimize("-O2")));
static inline void shadow_page_walk_end(struct kvm_vcpu *vcpu)
{
	kvm_emp_walk_shadow_page_lockless_end(vcpu);
}

static inline void emp_release_page_clean(struct page *p)
{
	if (!PageCompound(p) || PageHead(p))
		kvm_release_page_clean(p);
}

/**
 * map_spte - Install shadow page table entry
 * @param kvm_vcpu working vcpu info
 * @param gpa guest virtual addr
 * @param page page structure
 * @param gfn guest frame number
 * @param spte_attr attribute of spte
 * @param sptep pointer of shadow page table entry
 * @param ms memory slot
 */
static inline void map_spte(struct kvm_vcpu *vcpu, struct page *p, gfn_t gfn,
		u64 spte_attr, u64 *sptep, struct kvm_memory_slot *ms)
{

	u64 pfn, spte;
	pfn = page_to_pfn(p);

	debug_map_spte(p);

	spte = READ_ONCE(*sptep);
	if (likely(!is_shadow_present_pte(spte))) {
		spte = (pfn << PAGE_SHIFT) | spte_attr;
		if (ms && (ms->flags & KVM_MEM_READONLY))
			spte &= ~PT_WRITABLE_MASK;
		WRITE_ONCE(*sptep, spte);
		kvm_emp_mmu_rmap_add(vcpu, ms, sptep, gfn, ACC_ALL);
	} else if (pfn != spte_to_pfn(spte)) {
		/*kvm_flush_remote_tlbs(vcpu->kvm);*/
		BUG();
	}
	emp_release_page_clean(p);
}

/**
 * emp_map_prefetch_sptes - Install shadow page table entries for prefetch sub-blocks in a block
 * @param kvm_vcpu working vcpu info
 * @param head head of the faulted block
 * @param sb_head head of the faulted sub-block
 * @param demand demand page
 * @param gpa guest virtual addr
 * @param sptep pointer of shadow page table entry
 *
 * @return the attribute of the shadow page table entry
 *
 * Install sptes for a block except a demand sub-block
 */
u64 emp_map_prefetch_sptes(struct kvm_vcpu *vcpu, struct emp_gpa *head, struct emp_gpa *sb_head,
		struct emp_gpa *demand, pgoff_t demand_off, gva_t gpa, u64 *sptep)
{
	struct emp_gpa *g;
	u64 spte_attr;
	struct page *p;
	gfn_t gfn = GPA_TO_GFN(gpa);
	int i, map_spte_count;
	long offset;
	unsigned int sb_order, sb_mask;
	bool cpf_prefetched = is_gpa_flags_set(head, GPA_PREFETCHED_CPF_MASK);

	sb_order = gpa_subblock_order(demand);
	sb_mask = gpa_subblock_mask(demand);

	debug_check_sptep(sptep);

	spte_attr = READ_ONCE(*sptep) & ~(SHADOW_EPT_MASK);
	map_spte_count = 0;

	shadow_page_walk_begin(vcpu);

	offset = -((long) gpa_block_offset(head, demand_off));
	for_each_gpas(g, head) {
#ifdef CONFIG_EMP_DEBUG_PROGRESS
		int debug_map_spte_record = 0;
#endif
		if (g == sb_head && !cpf_prefetched) {
			offset += gpa_subblock_size(g);
			continue;
		}

		p = g->local_page->page;
		//offset = ((g - demand) * (1 << sb_order)) - (demand_off & sb_mask);
		for (i = 0; i < gpa_subblock_size(g); i++, offset++) {
			if (offset == 0)
				continue;

			if (map_spte_count++ % PTE_PREFETCH_NUM == 0)
				kvm_emp_mmu_topup_memory_caches(vcpu);
#ifdef CONFIG_EMP_DEBUG_PROGRESS
			if (debug_map_spte_record == 0) {
				debug_progress(g, (!is_shadow_present_pte(READ_ONCE(*sptep))) ? 1 : 0);
				debug_map_spte_record = 1;
			}
#endif
			map_spte(vcpu, p + i, gfn + offset,
					spte_attr, sptep + offset, NULL);
		}
	}

	shadow_page_walk_end(vcpu);

	return spte_attr;
}

/**
 * __emp_map_prefetch_sptes_memslot - Install shadow page table entries for prefetch pages using memslot
 * @param kvm_vcpu working vcpu info
 * @param s start page of the faulted block
 * @param e end page of the faulted block
 * @param sgfn guest physical frame number
 * @param sptep pointer of shadow page table entry
 * @param spte_attr attribute of spte
 * @param skip index of the processing pages
 *
 * Install sptes for prefetch pages using memslot
 */
static void __emp_map_prefetch_sptes_memslot(struct kvm_vcpu *vcpu,
	struct emp_gpa *s, struct emp_gpa *e, gfn_t sgfn, u64 *sptep, u64 spte_attr, int skip)
{
	struct emp_gpa *g;
	struct local_page *l;
	struct kvm_memory_slot *ms;
	gfn_t target_gfn = sgfn;
	int i, offset, map_spte_count;

	debug_BUG_ON(!s->local_page);
	map_spte_count = 0;
	offset = 0;

	for (g = s; g != e; g++) {
#ifdef CONFIG_EMP_DEBUG_PROGRESS
		int debug_map_spte_record = 0;
#endif
		l = g->local_page;
		for (i = 0; i < gpa_subblock_size(g); i++, target_gfn++, offset++) {
			ms = gfn_to_memslot(vcpu->kvm, target_gfn);
			if (!ms) {
				emp_release_page_clean(l->page + i);
				continue;
			}
			if (map_spte_count++ % PTE_PREFETCH_NUM == 0)
				kvm_emp_mmu_topup_memory_caches(vcpu);
#ifdef CONFIG_EMP_DEBUG_PROGRESS
			if (debug_map_spte_record == 0) {
				debug_progress(g, (!is_shadow_present_pte(READ_ONCE(*(sptep + offset)))) ? 1 : 0);
				debug_map_spte_record = 1;
			}
#endif
			map_spte(vcpu, l->page + i, target_gfn,
					spte_attr, sptep + offset, ms);
		}
	}
}

/**
 * emp_map_prefetch_sptes2 - Install shadow page table entries for prefetch pages in a block
 * @param kvm_vcpu working vcpu info
 * @param head head of the faulted block
 * @param demand demand page
 * @param gpa guest virtual addr
 * @param sptep pointer of shadow page table entry
 * @param slot memory slot
 *
 * Install sptes for a block except a demand page
 */
void emp_map_prefetch_sptes2(struct kvm_vcpu *vcpu, struct emp_gpa *head,
		struct emp_gpa *demand, pgoff_t demand_off, gva_t gpa, u64 *sptep,
		struct kvm_memory_slot *slot)
{
	int offset, sb_offset, map_spte_count;
	struct emp_gpa *g, *end, *mapping_sg, *mapping_eg;
	gfn_t gfn = GPA_TO_GFN(gpa), head_gfn;
	u64 spte_attr = READ_ONCE(*sptep) & ~(SHADOW_EPT_MASK);
	u64 *head_sptep;
	struct local_page *l;
	unsigned int sb_order, sb_mask;

	sb_order = gpa_subblock_order(demand);
	sb_mask = gpa_subblock_mask(demand);

	mapping_sg = head;
	mapping_eg = end = head + num_subblock_in_block(head);

	head_gfn = gfn - ((demand - head) << sb_order) - (demand_off & sb_mask);
	head_sptep = sptep - ((demand - head) << sb_order) - (demand_off & sb_mask);
	if (slot->base_gfn > 0 && head_gfn < slot->base_gfn)
		mapping_sg += ((slot->base_gfn - head_gfn) >> sb_order);
	if (head_gfn + gpa_block_size(head) > slot->base_gfn + slot->npages)
		mapping_eg -= (((head_gfn + gpa_block_size(head)) -
				(slot->base_gfn + slot->npages)) >> sb_order);

	debug_BUG_ON(!head->local_page);

	l = NULL;
	sb_offset = 0;
	map_spte_count = 0;

	shadow_page_walk_begin(vcpu);

	for (g = head; g < mapping_eg; g++) {
#ifdef CONFIG_EMP_DEBUG_PROGRESS
		int debug_map_spte_record = 0;
#endif
		if (g < mapping_sg)
			continue;

		l = g->local_page;
		offset = ((g - demand) << sb_order) - (demand_off & sb_mask);

		for (sb_offset = 0; sb_offset < gpa_subblock_size(g);
				sb_offset++, offset++) {
			if (sb_offset == (demand_off & sb_mask))
					continue;
			if (map_spte_count++ % PTE_PREFETCH_NUM == 0)
				kvm_emp_mmu_topup_memory_caches(vcpu);
#ifdef CONFIG_EMP_DEBUG_PROGRESS
			if (debug_map_spte_record == 0) {
				debug_progress(g, (!is_shadow_present_pte(READ_ONCE(*(sptep + offset)))) ? 1 : 0);
				debug_map_spte_record = 1;
			}
#endif
			map_spte(vcpu, l->page + sb_offset, gfn + offset,
					spte_attr, sptep + offset, NULL);
		}
	}

	if (unlikely(end != mapping_eg)) {

		debug_emp_map_prefetch_sptes2(g, mapping_eg);

		offset = ((mapping_eg - demand) << sb_order) - (demand_off & sb_mask);

		__emp_map_prefetch_sptes_memslot(vcpu, mapping_eg, end, gfn + offset,
				sptep + offset, spte_attr, 0);
	}

	if (unlikely(head != mapping_sg)) {
		__emp_map_prefetch_sptes_memslot(vcpu, head, mapping_sg,
				head_gfn, head_sptep, spte_attr, 0);
	}

	shadow_page_walk_end(vcpu);
}

/**
 * wait_fetching_except - Wait the completion of fetch work request except a specific subblock
 * @param b bvma data structure
 * @param c working vcpu ID
 * @param head head of block
 * @param except except subblock
 */
static void wait_fetching_except(struct emp_mm *b, struct vcpu_var *cpu, 
				 struct emp_gpa *head, struct emp_gpa *except)
{
	bool cpf_prefetched = is_gpa_flags_set(head, GPA_PREFETCHED_CPF_MASK);
	struct emp_gpa *g;

	for_each_gpas(g, head) {
		if (g == except && !cpf_prefetched)
			continue;

		if (!(b->sops.wait_read_async)(b, cpu, g))
			continue;

		/* Called from emp_page_fault_gpa(). Forking on EPT is not allowed. */
		debug_BUG_ON(is_gpa_remote_page_cow(g));

		clear_gpa_flags_if_set(g, GPA_REMOTE_MASK);
	}
}

static int install_sptes_for_subblock(struct kvm_vcpu *vcpu, struct emp_mm *bvma,
		struct emp_gpa * sb_head, struct emp_gpa *demand, pgoff_t demand_off, gva_t gpa,
		const int level, bool prefault, bool write_fault,
		bool writable, struct kvm_memory_slot *slot, u64 **sptepp);

/**
 * emp_page_fault_sptes_map - Mapping shadow page table entries for a block
 * @param kvm_vcpu working vcpu info
 * @param bvma bvma data structure
 * @param sb_head sub-block head
 * @param demand demand page of the faulted addr
 * @param gpa guest virtual address of fault addr
 * @param sptep pointer of shadow page table entry
 *
 * Install sptes for a block
 */
u64 emp_page_fault_sptes_map(struct kvm_vcpu *kvm_vcpu, struct vcpu_var *cpu,
		struct emp_gpa *head, u64 head_gpa, struct kvm_memory_slot *slot)
{
	struct emp_mm *bvma = get_kvm_emp_mm(kvm_vcpu->kvm);
	struct emp_gpa *pf_sb_head = NULL;
	unsigned long pf_gpa;
	u64 *sptep, mapping_attr;
	unsigned int sb_order;
	int demand_offset;

	sb_order = gpa_subblock_order(head);
	demand_offset = head->local_page->demand_offset;
	pf_sb_head = head + (demand_offset >> sb_order);
	wait_fetching_except(bvma, cpu, head, pf_sb_head);

	pf_gpa = head_gpa + (demand_offset << PAGE_SHIFT);
	sptep = kvm_emp___get_spte(kvm_vcpu, PG_LEVEL_4K,
				pf_gpa >> PAGE_SHIFT);
	if (sptep == NULL || !is_shadow_present_pte(*sptep)) {
		if (!is_shadow_present_pte(*sptep))
			emp_get_subblock(pf_sb_head, false);

		install_sptes_for_subblock(kvm_vcpu, bvma, 
				pf_sb_head, pf_sb_head, demand_offset, pf_gpa,
				PG_LEVEL_4K, false, true, true, slot,
				&sptep);
		head->local_page->sptep = sptep - demand_offset;
	}
	mapping_attr = emp_map_prefetch_sptes(kvm_vcpu, head, pf_sb_head,
				pf_sb_head, demand_offset, pf_gpa, sptep);

	head->local_page->demand_offset = 0;

	return mapping_attr;
}

/**
 * __mmu_set_spte - Install a shadow page table entry
 * @param kvm_vcpu working vcpu info
 * @param sptep pointer of a shadow page table entry
 * @param pte_access pte_access
 * @param write_fault write page fault?
 * @param level page table level
 * @param gpa guest virtual address of fault addr
 * @param pfn physical frame number of fault addr
 * @param prefault prefault?
 * @param writable writable page?
 * 
 * @retval return value for page fault handles (EMULATE, RETRY, INVALID)
 */
static inline int __mmu_set_spte(struct kvm_vcpu *vcpu, u64 *sptep,
		unsigned pte_access, int write_fault,
		int level, gva_t gpa, kvm_pfn_t pfn, bool prefault,
		bool writable)
{
	int ret;
	debug___mmu_set_spte(pfn);

	ret = kvm_emp_mmu_set_spte(vcpu, sptep, pte_access, write_fault, level,
			GPA_TO_GFN(gpa), pfn, prefault, writable);
	emp_release_page_clean(pfn_to_page(pfn));
	return ret;
}

#ifdef CONFIG_EMP_OPT
/**
 * get_next_pt_premapping - Prepare next page mapping
 * @param vcpu working vcpu info
 * @param slot memory slot
 * @param gpa guest physical address
 * @param level page table level
 * @param min preparing number
 */
static void get_next_pt_premapping(struct kvm_vcpu *vcpu,
		struct kvm_memory_slot *slot, gva_t gpa, int level, int min)
{
	int i, ret;
	kvm_pfn_t target_gfn = GPA_TO_GFN(gpa + HPAGE_SIZE);

	for (i = 0; i < min; i++) {
		kvm_emp_make_mmu_pages_available(vcpu);
		ret = kvm_emp___prepare_map(vcpu, &level, gpa + HPAGE_SIZE,
				&target_gfn, true, NULL, NULL);
		if (ret == EMP_PF_RETRY)
			break;
		else if (ret == EMP_PF_RETURN_ESC_LEVEL) {
			printk(KERN_WARNING "__prepare_map does not reach the target level");
			continue;
		}

		kvm_emp_mmu_topup_memory_caches(vcpu);
		target_gfn += (1 << (HPAGE_SHIFT - PAGE_SHIFT));
		if (target_gfn < slot->base_gfn ||
				target_gfn >= (slot->base_gfn + slot->npages))
			break;
	}
}
#endif /* CONFIG_EMP_OPT */
/**
 * fast_install_spte - Mapping demand shadow page table entry
 * @param kvm_vcpu working vcpu info
 * @param bvma bvma data structure
 * @param sb_head sub-block head
 * @param demand demand page of the faulted addr
 * @param gpa guest virtual address of fault addr
 * @param level page table level
 * @param prefault prefault?
 * @param write_fault write page fault?
 * @param writable writable page?
 * @param slot memory slot
 * @param sptepp pointer of shadow page table entry
 
 * @retval return value for page fault handles (EMULATE, RETRY, INVALID)
 */
static int fast_install_spte(struct kvm_vcpu *vcpu, struct emp_mm *bvma,
		struct emp_gpa *demand, gva_t gpa, u64 pfn,
		const int level, bool prefault, bool write_fault,
		bool writable, struct kvm_memory_slot *slot, u64 **sptepp)
{
	u64 *sptep, spte;
	gfn_t gfn = GPA_TO_GFN(gpa);
	bool installed = false;
	int ret = RET_PF_RETRY;

	// kvm_mm may require pte_list_desc
	// need more optimization?
	kvm_emp_mmu_topup_memory_caches(vcpu);

	// follow fast page fault of kvm
	shadow_page_walk_begin(vcpu);

	sptep = kvm_emp___get_spte(vcpu, level, gfn);
	if (sptep == NULL) {
		debug_progress(demand, 0);
		debug_progress(emp_get_block_head(demand), demand - emp_get_block_head(demand));
		goto slow_path;
	}

	if (kvm_emp_get_shadow_page_level(__pa(sptep)) != PG_LEVEL_4K) {
		debug_progress(demand, 0);
		debug_progress(emp_get_block_head(demand), demand - emp_get_block_head(demand));
		goto slow_path;
	}

	spte = READ_ONCE(*sptep);
	debug_progress(demand, spte);
	debug_progress(demand, pfn);
	debug_progress(emp_get_block_head(demand), spte);
	debug_progress(emp_get_block_head(demand), pfn);
	installed = true;

	if (likely(spte == 0)) {
#ifdef CONFIG_EMP_DEBUG_PROGRESS
/* from kvm/mmu.h */
#define PT_PAGE_SIZE_SHIFT 7
#define PT_PAGE_SIZE_MASK (1ULL << PT_PAGE_SIZE_SHIFT)
/* from is_large_pte(pte) of kvm/mmu/spte.h */
#define ____is_large_pte(pte) ((pte) & PT_PAGE_SIZE_MASK)
		int debug_is_shadow = is_shadow_present_pte(spte) ? 1 : 0;
		int debug_level_high = level > PG_LEVEL_4K ? 1 : 0;
		int debug_is_large = ____is_large_pte(spte);
		int debug_pfn_same = (pfn == spte_to_pfn(spte)) ? 1 : 0;
		int debug_was_rmapped = 0;
		if (debug_is_shadow) {
			if (debug_level_high && !debug_is_large)
				debug_was_rmapped = 0;
			else if (!debug_pfn_same)
				debug_was_rmapped = 0;
			else
				debug_was_rmapped = 1;
		}
		debug_progress(demand, (debug_is_shadow ? 0x10000ULL : 0x0ULL)
					| (debug_level_high ? 0x1000ULL : 0x0ULL)
					| (debug_is_large ? 0x100ULL : 0x0ULL)
					| (debug_pfn_same ? 0x10ULL : 0x0ULL)
					| (debug_was_rmapped ? 0x1ULL : 0x0ULL));
#endif
		ret = __mmu_set_spte(vcpu, sptep, ACC_ALL, write_fault,
				level, gpa, pfn, prefault, writable);
	} else if (pfn == spte_to_pfn(spte))
		emp_release_page_clean(pfn_to_page(pfn));
	else if (pfn != spte_to_pfn(spte)) {
		kvm_flush_remote_tlbs(vcpu->kvm);
		installed = false;
		BUG();
	}

slow_path:
	shadow_page_walk_end(vcpu);

	if (installed)
		goto out;

	// fallback
	lock_kvm_mmu_lock(vcpu->kvm);
	kvm_emp_make_mmu_pages_available(vcpu);
	ret = kvm_emp___direct_map(vcpu, gpa, write_fault, writable, level,
			pfn, prefault, slot);
#ifdef CONFIG_EMP_OPT
	if (bvma->config.next_pt_premapping)
		get_next_pt_premapping(vcpu, slot, gpa, level, 3);
#endif
	unlock_kvm_mmu_lock(vcpu->kvm);
	emp_release_page_clean(pfn_to_page(pfn));

	sptep = kvm_emp___get_spte(vcpu, level, gfn);
out:
	*sptepp = sptep;

	return ret;
}

/**
 * map_sptes_in_subblock - Mapping shadow page table entries for a sub-block except demand page
 * @param kvm_vcpu working vcpu info
 * @param bvma bvma data structure
 * @param sb_head sub-block head
 * @param demand demand page of the faulted addr
 * @param gpa guest virtual address of fault addr
 * @param sptep pointer of shadow page table entry
 *
 * Install a sub-block without mapped demand page
 */
void map_sptes_in_subblock(struct kvm_vcpu *vcpu, struct emp_mm *bvma,
		struct emp_gpa *sb_head, struct emp_gpa *demand, pgoff_t demand_off,
		gva_t gpa, u64 *sptep)
{
	struct page *p;
	u64 spte_attr;
	gfn_t gfn = GPA_TO_GFN(gpa);
	int offset, sb_offset, map_spte_count;
	int sb_mask;
#ifdef CONFIG_EMP_DEBUG_PROGRESS
	int debug_map_spte_record = 0;
#endif

	sb_mask = gpa_subblock_mask(demand);

	debug_map_sptes_in_subblock(sb_head, sptep);

	spte_attr = READ_ONCE(*sptep) & ~(SHADOW_EPT_MASK);
	map_spte_count = 0;

	shadow_page_walk_begin(vcpu);

	p = sb_head->local_page->page;

	/* install sub-block without mapped demand page */
	for (sb_offset = 0; sb_offset < gpa_subblock_size(sb_head);
			sb_offset++) {
		if (sb_offset == (demand_off & sb_mask))
			continue;

		offset = sb_offset - (int) (demand_off & sb_mask);

		if (map_spte_count++ % PTE_PREFETCH_NUM == 0)
			kvm_emp_mmu_topup_memory_caches(vcpu);
#ifdef CONFIG_EMP_DEBUG_PROGRESS
		if (debug_map_spte_record == 0) {
			debug_progress(sb_head, (!is_shadow_present_pte(READ_ONCE(*sptep))) ? 0x1 : 0x0);
			debug_map_spte_record = 1;
		}
#endif
		map_spte(vcpu, p + sb_offset, gfn + offset,
				spte_attr, sptep + offset, NULL);
	}

	shadow_page_walk_end(vcpu);
}

/**
 * install_sptes_for_subblock - Install shadow page table entries for a sub-block
 * @param kvm_vcpu working vcpu info
 * @param bvma bvma data structure
 * @param sb_head sub-block head
 * @param demand demand page of the faulted addr
 * @param gpa guest virtual address of fault addr
 * @param level page table level
 * @param prefault prefault?
 * @param write_fault write page fault?
 * @param writable writable page?
 * @param slot memory slot
 * @param sptepp pointer of shadow page table entry
 *
 * @retval return value for page fault handles (EMULATE, RETRY, INVALID)
 *
 * Install shadow page table entry for a sub-block in the order below
 * + (1) it installs a demand page in a sub-block
 * + (2) it installs remained pages in a sub-block
 */
static int install_sptes_for_subblock(struct kvm_vcpu *kvm_vcpu,
		struct emp_mm *bvma, struct emp_gpa *sb_head,
		struct emp_gpa *demand, pgoff_t demand_off, gva_t gpa,
		const int level, bool prefault, bool write_fault,
		bool writable, struct kvm_memory_slot *slot, u64 **sptepp)
{
	u64 *sptep, pfn;
	unsigned int sb_offset;
	int ret = 0;
	struct page *demand_p;
	struct emp_gpa *head = emp_get_block_head(sb_head);
	bool cpf_prefetched = is_gpa_flags_set(head, GPA_PREFETCHED_CPF_MASK);
	int vcpu_id = kvm_vcpu->vcpu_id + VCPU_START_ID;
	struct vcpu_var *vcpu = &bvma->vcpus[vcpu_id];
	bool enable_transition_csf = bvma_transition_csf(bvma);

	sb_offset = demand_off & gpa_subblock_mask(demand);
	demand_p = sb_head->local_page->page + sb_offset;
	pfn = page_to_pfn(demand_p);

	/* demand page install */
	ret = fast_install_spte(kvm_vcpu, bvma, demand, gpa, pfn, level,
			false, write_fault, writable, slot, &sptep);
	debug_check_sptep(sptep);
	if (cpf_prefetched) { 
		/* if the work request is completed, change the block to CSF */
		if (enable_transition_csf &&
				(bvma->sops.try_wait_read_async(bvma, vcpu, sb_head))) {
			clear_gpa_flags_if_set(sb_head, GPA_REMOTE_MASK);
			map_sptes_in_subblock(kvm_vcpu, bvma, sb_head, demand, demand_off, 
									gpa, sptep);
			clear_gpa_flags_if_set(head, GPA_PREFETCHED_CPF_MASK);
			set_gpa_flags_if_unset(head, GPA_PREFETCHED_CSF_MASK);
#ifdef CONFIG_EMP_STAT
			bvma->stat.cpf_to_csf_transition++;
#endif
		}
	} else {
		/* demand sub-block install */
		map_sptes_in_subblock(kvm_vcpu, bvma, sb_head,
				      demand, demand_off, gpa, sptep);
	}

	*sptepp = sptep;

	return ret;
}

/**
 * emp_install_sptes - Installs shadow page table entries for gpas in a block
 * @param kvm_vcpu working vcpu info
 * @param bvma bvma data structure
 * @param vcpu_id working vcpu ID
 * @param head head of the faulted block
 * @param demand demand page of the faulted addr
 * @param gpa guest virtual address of fault addr
 * @param level page table level
 * @param prefault prefault?
 * @param write_fault write page fault?
 * @param writable writable page?
 * @param slot memory slot
 * @param fetch fetched block?
 *
 * @retval return value for page fault handles (EMULATE, RETRY, INVALID)
 */
static int emp_install_sptes(struct kvm_vcpu *vcpu, struct emp_mm *bvma,
		struct vcpu_var *cpu, struct emp_gpa *head,
		struct emp_gpa *demand, pgoff_t demand_off, gva_t gpa,
		int level, bool prefault, bool write_fault, bool writable,
		struct kvm_memory_slot *slot, bool fetch)
{
	gfn_t head_gfn;
	u64 *sptep, pfn;
	int sb_offset;
	struct emp_gpa *sb_head;
	bool multiple_ms = false;
	unsigned int sb_order, sb_mask;
	int ret;

	sb_order = gpa_subblock_order(demand);
	sb_mask = gpa_subblock_mask(demand);
	sb_offset = demand_off & sb_mask;
	sb_head = demand;

	head_gfn = GPA_TO_GFN(gpa) - ((demand - head) << sb_order) - sb_offset;
	// check how many memslots are covered by a block
	if ((slot->base_gfn > 0 && head_gfn < slot->base_gfn) ||
			((head_gfn + gpa_block_size(head)) >
			 (slot->base_gfn + slot->npages)))
		multiple_ms = true;

	if (likely(!multiple_ms)) {
		ret = install_sptes_for_subblock(vcpu, bvma, sb_head, demand,
				demand_off, gpa, level, prefault, write_fault,
				writable, slot, &sptep);
		debug_check_sptep(sptep);

		if (!fetch)
			emp_map_prefetch_sptes(vcpu, head, sb_head, demand,
					demand_off, gpa, sptep);
		goto out;
	}

	pfn = page_to_pfn(sb_head->local_page->page + sb_offset);
	ret = fast_install_spte(vcpu, bvma, demand, gpa, pfn, level,
			false, write_fault, writable, slot, &sptep);
	debug_check_sptep(sptep);

	if (fetch) {
		wait_fetching_except(bvma, cpu, head, sb_head);
		clear_gpa_flags_if_set(head, GPA_PREFETCHED_MASK);
	}
	emp_map_prefetch_sptes2(vcpu, head, demand, demand_off, gpa, sptep, slot);
out:
	head->local_page->sptep = sptep - ((demand - head) << sb_order) - sb_offset;
	set_gpa_flags_if_unset(head, GPA_EPT_MASK);
	return ret;
}

/**
 * emp_fetch_barrier - Guarantees that fetching in-progress is completed
 * @param vcpu working vcpu info
 * @param hva host virtual address of faulted addr
 * @param gva guest virtual address of faulted addr
 *
 * @retval 0: Error
 * @retval 1: Success
 */
static int emp_fetch_barrier(struct kvm_vcpu *kvm_vcpu, const unsigned long hva,
			     gva_t gva)
{
	struct emp_mm *bvma = get_kvm_emp_mm(kvm_vcpu->kvm);
	int vcpu_id = kvm_vcpu->vcpu_id + VCPU_START_ID;
	struct vcpu_var *vcpu = &bvma->vcpus[vcpu_id];
	struct emp_gpa *gpa;
	struct emp_vmr *vmr;
	pgoff_t gpa_off;
	struct emp_gpa *head;
#ifdef CONFIG_EMP_BLOCK
	bool csf = bvma_csf_enabled(bvma);
	bool cpf = bvma_cpf_enabled(bvma);
	int fallback;
#endif
	unsigned long sb_index;
	int gpa_sb_offset;

	/* returns NULL for invalid hva */
	if ((vmr = emp_vmr_lookup_hva(bvma, hva)) == NULL)
		return 0;

	/* retrieve gpa and block */
	gpa_off = GPN_OFFSET(bvma, HVA_TO_GPN(bvma, vmr, hva));
	sb_index = gpa_off >> bvma_subblock_order(bvma);
	gpa = get_exist_gpadesc(vmr, sb_index);
	head = emp_get_block_head(gpa);
	gpa_sb_offset = gpa_off & gpa_subblock_mask(head);

	/* waiting for fetchings to be completed. */
	if (head->r_state != GPA_FETCHING)
		return 0;

#ifdef CONFIG_EMP_BLOCK
	if (csf && !is_gpa_flags_set(head, GPA_PREFETCH_ONCE_MASK)) {
		if (cpf) {
			/* waiting only for demand page */
			fallback = bvma->sops.wait_read_async_demand_page(bvma, vcpu,
									  gpa, gpa_sb_offset);

			/* processed only a page, not all */
			head->local_page->demand_offset = gpa_block_offset(head, gpa_off);
			if (fallback > 1)
				set_gpa_flags_if_unset(head, GPA_PREFETCHED_CSF_MASK);
			else
				set_gpa_flags_if_unset(head, GPA_PREFETCHED_CPF_MASK);
			set_gpa_flags_if_unset(head, GPA_PREFETCH_ONCE_MASK);
		} else {
			/* waiting only for demand subblock */
			if ((bvma->sops.wait_read_async)(bvma, vcpu, gpa)) {
				/* Called from emp_page_fault_gpa(). Forking on EPT is not allowed. */
				debug_BUG_ON(is_gpa_remote_page_cow(gpa) == true);
				clear_gpa_flags_if_set(gpa, GPA_REMOTE_MASK);
			}

			/* processed only a sub-block, not all */
			if (gpa_subblock_order(head) != head->block_order) {
				head->local_page->demand_offset = gpa_block_offset(head, gpa_off);
				set_gpa_flags_if_unset(head, GPA_PREFETCHED_CSF_MASK);
				set_gpa_flags_if_unset(head, GPA_PREFETCH_ONCE_MASK);
			}
		}
		goto out;
	}
#endif
	/* At this point, gpa is already in lru lists 
	 * you don't have to insert this gpa into active list */
	/* if GPA fetching is in progress, we have to wait(barrier). */
	wait_fetching_except(bvma, vcpu, head, NULL);
#ifdef CONFIG_EMP_BLOCK
out:
#endif
	return 1;
}

/**
 * calibrate_block_count - calibrate block's reference count
 * @param head head of block
 * @param fs first gpa for fetching (Fetch Start)
 * @param fe last gpa for fetching (Fetch End)
 */
void calibrate_block_count(struct emp_gpa *head, struct emp_gpa *fs, struct emp_gpa *fe)
{
	struct emp_gpa *g;

	switch (head->r_state) {
		case GPA_ACTIVE:
			/* Page fault has occured.
			 * However, GPA was already in local memory.
			 *  (1) GPA was in inactive list.
			 *  (2) GPA was in writeback state.
			 *  (3) GPA was in active list
			 *     (simulatneous page fault by two vCPUs). 
			 *
			 * In these cases, GPA does not have to be fetched and
			 * its state is just changed to GPA_ACTIVE. However, 
			 * they have to be handled for the page table mapping process. */
			for_each_gpas(g, head) {
				/* For the pages to be installed in the page table,
				 * its page reference count should be increased because
				 * the page reference count will be decreased during mapping.
				 * kvm_release_pfn_clean() calls put_page(). */
				emp_get_subblock_calibrate(g);
			}
			break;

		case GPA_FETCHING:
			for_each_gpas(g, head) {
				if (g < fs || fe <= g)
					emp_get_subblock_calibrate(g);
			}

			break;

		default:
			printk("head's r_state is %d\n", head->r_state);
			BUG();
	}
}

/**
 * emp_page_fault_gpa - (*Entry Point*) EPT page fault handling function
 * @param kvm_vcpu working vcpu info
 * @param hva host virtual address of faulted addr
 * @param write_fault write page fault?
 * @param writable writable page?
 * @param pfn physical frame number of faulted addr
 * @param gva guest virtual address of faulted addr
 * @param level level of page table
 * @param error_code error descriptor
 * @param slot memory slot
 *
 * @retval return value for page fault handler (EMULATE, RETRY, INVALID)
 *
 * This function fetches faulting pages and update lru lists
 */
int COMPILER_DEBUG
emp_page_fault_gpa(struct kvm_vcpu *kvm_vcpu, const unsigned long hva, 
		bool write_fault, bool *writable, u64 *pfn, const gva_t gva,
		int level, u32 error_code, bool prefault,
		struct kvm_memory_slot *slot)
{
	struct emp_mm *bvma = get_kvm_emp_mm(kvm_vcpu->kvm);
	int vcpu_id = kvm_vcpu->vcpu_id + VCPU_START_ID;
	struct vcpu_var *cpu = &bvma->vcpus[vcpu_id];
	struct emp_gpa *demand, *head;
	struct emp_gpa *fs, *fe; // fetch_start, fetch_end
	struct emp_vmr *vmr;
	pgoff_t demand_off;
	unsigned long demand_sb_off;
	unsigned long head_idx;
	unsigned long sb_mask;
	unsigned int sb_order;
	bool read_only_mapping;
	bool fetch;
	int ret = RET_PF_RETRY, r;
#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
	s64 __num_emp_gpa_fault;
#endif

	/* returns error for invalid HVAs */
	if ((vmr = emp_vmr_lookup_hva(bvma, hva)) == NULL)
		return -EINVAL;

	/* For low memory region, let KVM handles */
	if (vmr->id == 0 && (hva - vmr->host_vma->vm_start) <= LOW_MEMORY_REGION_SIZE)
		return -EINVAL;

#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
	__num_emp_gpa_fault = atomic64_inc_return(&num_emp_gpa_fault);
	if ((__num_emp_gpa_fault == 1) ||
			(__num_emp_gpa_fault % CONFIG_EMP_SHOW_FAULT_PROGRESS_PERIOD == 0))
		printk(KERN_ERR "[EMP_PROGRESS] %s num_gpa_fault: %lld "
				"emm: %d vmr: %d hva: 0x%lx gva: 0x%lx\n",
				__func__, __num_emp_gpa_fault,
				bvma->id, vmr->id, hva, gva);
#endif


#ifdef CONFIG_EMP_STAT
	/* update stat */
	inc_vma_fault(cpu);
#endif	
	/* hva to gpa */
	sb_order = bvma_subblock_order(bvma);
	demand_off = GPN_OFFSET(bvma, HVA_TO_GPN(bvma, vmr, hva));
	demand_sb_off = demand_off >> sb_order;
	demand = get_gpadesc(vmr, demand_sb_off);
	if (unlikely(!demand))
		return VM_FAULT_SIGBUS;

	sb_mask = gpa_subblock_mask(demand);

	emp_wait_for_writeback(bvma, cpu, gpa_block_size(demand));

	// gpa lock will be released by barr_fetch
	head = emp_lock_block(vmr, &demand, demand_sb_off);
	debug_BUG_ON(!head); // we already have @demand
	debug_progress(head, (((u64) error_code) << 32)
				| ((level & 0xff) << 16)
				| (prefault ? 0x10 : 0)
				| (write_fault ? 0x1 : 0));
	fs = head;
	fe = head + num_subblock_in_block(head);
	/* Assume that gpa descriptors in a block reside on a contiguous memory */
	head_idx = demand_sb_off - (demand - head);

#ifdef CONFIG_EMP_BLOCK
	if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK)) {
		debug_emp_page_fault_gpa(head);

		if (is_gpa_flags_set(head, GPA_HPT_MASK)) {
			struct vm_fault vmf = {
				.vma = vmr->host_vma,
				.pgoff = GPN_OFFSET(bvma, HVA_TO_GPN(bvma, vmr, hva)),
				.address = hva
			};

			vmf.flags = FAULT_FLAG_WRITE;
			emp_page_fault_hptes_map(bvma, vmr, head, demand, demand_sb_off,
						 fs, fe, true, &vmf, true);
		} else if (is_gpa_flags_set(head, GPA_EPT_MASK)) {
			u64 mapping_attr;
			u64 head_gpa = (gva & PAGE_MASK) - 
				((demand - head) << (sb_order + PAGE_SHIFT)) -
				((demand_off & sb_mask) << PAGE_SHIFT);

			mapping_attr = emp_page_fault_sptes_map(kvm_vcpu,
					cpu, head, head_gpa, slot);

			if ((write_fault == false) ||
				(mapping_attr & PT_WRITABLE_MASK)) {
				clear_gpa_flags_if_set(head, GPA_PREFETCHED_MASK);
#ifdef CONFIG_EMP_STAT
				bvma->stat.csf_fault++;
#endif
				goto return_to_fault_inst;
			}
		}

		clear_gpa_flags_if_set(head, GPA_PREFETCHED_MASK);
#ifdef CONFIG_EMP_STAT
		bvma->stat.csf_fault++;
#endif
	}
#endif

	debug_emp_page_fault_gpa2(head);

#ifdef CONFIG_EMP_IO
	// wait for completion of hva fault handling
	if (is_gpa_flags_set(head, GPA_IO_IP_MASK)) {
		struct page *page;
		debug_BUG_ON(!head->local_page);
		debug_BUG_ON(!head->local_page->page);
		page = head->local_page->page;
		emp_unlock_block(head);

		wait_on_page_locked(page);

		head = emp_lock_block_local(vmr, &demand);
		head_idx = demand_sb_off - (demand - head);
	}
#endif
	
	// we must check whether the block need to be mapped with read-only,
	// because we will discard ZERO_BLOCK_MASK in fetch_block. read-only
	// block reduces writeback request to 2nd-tier memory
	// prevent stretched block being mapped with read-only permission
	read_only_mapping = (write_fault == false) &&
		!(get_gpa_flags(head) & (GPA_HPT_MASK | GPA_DIRTY_MASK));

	/* two cases exist if (head->r_state != GPA_INIT).
	 * 1. the gpa is removed from victim_list and added to active_list again.
	 * 2. Another processor handles the identical fault. 
	 *    It should wait for the completion.  	*/
	if (head->r_state != GPA_INIT) {
#ifdef CONFIG_EMP_STAT
		inc_local_fault(cpu);
#endif
		r = handle_local_fault(vmr, &head, demand, cpu, NULL, NULL);
		if (unlikely(r < 0)) {
			ret = r;
			goto skip_install_sptes;
		}
	} else {
		r = handle_remote_fault(vmr, &head, head_idx,
					demand, demand_off,
					cpu, read_only_mapping);
		if (unlikely(r < 0)) {
			clear_in_flight_fetching_block(vmr, cpu, head);
			ret = r;
			goto skip_install_sptes;
		}
	}

	/* pfn will be passed to caller function and it will be mapped if the
	 * mapping operation is failed in emp (pfn will be mapped to) */
	*pfn = page_to_pfn(demand->local_page->page + (demand_off & sb_mask));

	debug_emp_page_fault_gpa3(head, gva, demand_off);

	emp_fetch_barrier(kvm_vcpu, hva, gva);
	
	/* calibrating page reference counts by calling get_page or put_page */
	calibrate_block_count(head, fs, fe);

	/* set all the pages writable in GPL version */
	*writable = true;

	if (head->r_state == GPA_FETCHING)
		head->r_state = GPA_ACTIVE;

	if (*writable) {
		set_gpa_flags_if_unset(head, GPA_DIRTY_MASK);
	}

	debug_emp_install_sptes(head);

	/* fetching completed. now install shadow page table entry */
	fetch = (is_gpa_flags_set(head, GPA_PREFETCHED_MASK))? true: false;
	ret = emp_install_sptes(kvm_vcpu, bvma, cpu, head, demand, demand_off,
			        gva, level, prefault, write_fault, *writable,
				slot, fetch);

	debug_emp_install_sptes2(bvma, head, demand);

	if (!is_gpa_flags_set(head, GPA_HPT_MASK)
			&& head->local_page->vmr_id != vmr->id)
		emp_update_rss_add_force(vmr, gpa_block_size(head),
					DEBUG_RSS_ADD_FAULT_GPA,
					head, DEBUG_UPDATE_RSS_BLOCK);

skip_install_sptes:
	emp_unlock_block(head);

	if (unlikely(ret < 0))
		printk(KERN_ERR "%s returns error. code: %d"
				" pgoff: %lx last_mr: %d local: %c remote: %llx\n",
				__func__, -ret,
				demand_off,
				demand ? demand->last_mr_id : -1,
				(demand && demand->local_page)
					? 'V' : 'N',
				demand ? get_gpa_remote_page_val(demand) : -1);
	return ret;
#ifdef CONFIG_EMP_BLOCK
/* defined(CONFIG_EMP_BLOCK || defined(CONFIG_EMP_EXT) */
return_to_fault_inst:
	emp_unlock_block(head);
	return RET_PF_RETRY;
#endif
}

int COMPILER_DEBUG
emp_lock_range_pmd(struct kvm *kvm, unsigned long hva,
			void **locked_vmr, void **locked_index)
{
	struct emp_vmr *vmr;
	struct emp_gpa *head;
	unsigned long start, idx, end;
	struct emp_mm *emm = get_kvm_emp_mm(kvm);
	unsigned long block_large;
	int gpa_index_order;

	if ((vmr = emp_vmr_lookup_hva(emm, hva)) == NULL)
		return EMP_KVM_LOCK_NOT_MINE;

	gpa_index_order = bvma_subblock_order(emm);
	block_large = GPN_OFFSET(emm, HVA_TO_GPN(emm, vmr, hva));
	block_large &= ~((1UL << (HPAGE_SHIFT - PAGE_SHIFT)) - 1);
	start = block_large >> gpa_index_order;

	*locked_vmr = (void *) vmr;
	*locked_index = (void *) start;

	head = get_gpadesc(vmr, start);
	if (unlikely(!head))
		return EMP_KVM_LOCK_BUSY;

	if (gpa_block_order(head) == (HPAGE_SHIFT - PAGE_SHIFT)) {
		head = emp_trylock_block(vmr, NULL, start);
		if (head)
			return 0;
		return EMP_KVM_LOCK_BUSY;
	}

	idx = start;
	end = start + (PTRS_PER_PMD >> gpa_index_order);
	while (idx < end) {
		head = emp_trylock_block(vmr, NULL, idx);
		if (!head)
			goto lock_failed;
		idx += num_subblock_in_block(head);
	}
	return 0;

lock_failed:
	end = idx;
	idx = start;
	while (idx < end) {
		head = get_exist_gpadesc(vmr, idx);
		idx += num_subblock_in_block(head);
		emp_unlock_block(head);
	}
	return EMP_KVM_LOCK_BUSY;
}

void COMPILER_DEBUG emp_unlock_range_pmd(struct kvm *kvm,
			void *locked_vmr, void *locked_index)
{
	struct emp_mm *emm = get_kvm_emp_mm(kvm);
	struct emp_gpa *head;
	struct emp_vmr *vmr = (struct emp_vmr *) locked_vmr;
	unsigned long start = (unsigned long) locked_index;
	unsigned long idx, end;
	int gpa_index_order;

	debug_BUG_ON(vmr == NULL); // the wrapper function should handle this

	/* TODO: set unlink flag */

	gpa_index_order = bvma_subblock_order(emm);
	idx = start;
	end = start + (PTRS_PER_PMD >> gpa_index_order);
	while (idx < end) {
		head = get_exist_gpadesc(vmr, idx);
		idx += num_subblock_in_block(head);
		emp_unlock_block(head);
	}
}
#endif /* CONFIG_EMP_VM */
