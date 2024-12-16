#include <linux/kthread.h>
#include <linux/wait.h>
#include <asm/fpu/api.h>
#include "vm.h"
#include "gpa.h"
#include "alloc.h"
#include "reclaim.h"
#include "block-flag.h"
#include "donor_mem_rw.h"
#include "debug.h"
#include "vcpu_var.h"
#include "pcalloc.h"

// we assume that TLB valid period without TLB shootdown is 20ms
#define TLB_VALID_PERIOD (10ULL * MS_TO_NS)
#define MRU_BUF_FULL(size, len) ((len) >= (size))

static int reclaim_gpa_many(struct emp_mm *, struct emp_gpa **, int,
			    struct emp_gpa **, struct emp_gpa **);
bool make_all_cpus_req(struct kvm *kvm, unsigned int req); /* gpa.c */

/**
 * __emp_list_head_local_page - Get the local page in the emp list at head
 * @param list emp_list
 *
 * @return local page of the head of list
 */
static inline struct local_page * COMPILER_OPT
__emp_list_head_local_page(struct emp_list *list)
{
	struct local_page *r;
	r = list_entry(emp_list_get_head(list), struct local_page, lru_list);
	return r;
}

/**
 * is_active_list_full - Check if the active list is full
 * @param bvma bvma data structure
 *
 * @retval true: Full
 * @retval false: Not full
 */
static inline bool is_active_list_full(struct emp_mm *bvma)
{
	size_t t = bvma->ftm.active_pages_len + NUM_VICTIM_CLUSTER;
	return ((size_t)atomic_read(&bvma->ftm.active_list.page_len) >= t);
}

/**
 * is_inactive_list_full - Check if the inactive list is full
 * @param bvma bvma data structure
 *
 * @retval true: Full
 * @retval false: Not full
 */
static inline bool is_inactive_list_full(struct emp_mm *bvma, int pressure)
{
	size_t t = bvma->ftm.inactive_pages_len - pressure;
	return ((size_t)read_inactive_list_page_len(bvma) >= t);
}

/**
 * refill_lru_buf - Refill the LRU buffer in the list
 * @param target_list target list
 * @param lb_head LRU buffer head
 * @param lb_size LRU buffer size
 *
 * @return the number of filled entries in the LRU buffer
 */
static int COMPILER_DEBUG
refill_lru_buf(struct emp_mm *emm, struct slru *target_list,
		struct emp_list *lru_list, int cpu_id, const int lb_size)
{
	struct list_head *cur, *n;
	int num_pop = 0, num_fail = 0;
	struct local_page *lp;
	struct temp_list to_lru;
	struct emp_list *global_list = &target_list->list;

	init_temp_list(&to_lru);

	emp_list_lock(global_list);
	emp_list_for_each_safe(cur, n, global_list) {
		lp = __get_local_page_from_list(cur);
		debug_assert(is_local_page_on_global(lp));
		if (emp_trylock_local_page(emm, lp) == NULL) {
			if ((++num_fail) >= lb_size)
				break;
			else
				continue;
		}
		emp_list_del(cur, global_list);
		temp_list_add_tail(cur, &to_lru);
		if ((++num_pop) >= lb_size)
			break;
	}
	emp_list_unlock(global_list);

	if (unlikely(num_pop == 0)) {
		struct emp_list *mru_list;
		num_fail = 0;
		mru_list = get_list_ptr_mru(emm, target_list, cpu_id);
		emp_list_lock(mru_list);
		emp_list_for_each_safe(cur, n, mru_list) {
			lp = __get_local_page_from_list(cur);
			debug_assert(is_local_page_on_mru(lp));
			if (emp_trylock_local_page(emm, lp) == NULL) {
				if ((++num_fail) >= lb_size)
					break;
				else
					continue;
			}
			emp_list_del(cur, mru_list);
			temp_list_add_tail(cur, &to_lru);
			if ((++num_pop) >= lb_size)
				break;
		}
		emp_list_unlock(mru_list);
	}

	emp_list_lock(lru_list);
	temp_list_for_each_safe(cur, n, &to_lru) {
		lp = __get_local_page_from_list(cur);
		set_local_page_cpu_lru(lp, cpu_id);
		temp_list_del(cur, &to_lru);
		emp_list_add_tail(cur, lru_list);
		emp_unlock_local_page(emm, lp);
	}
	emp_list_unlock(lru_list);

	return num_pop;
}

/**
 * flush_active_mru - Update entries in the active MRU buffer
 * @param emm emm data structure
 * @param target_list LRU list that need to update MRU buffer
 * @param mru_list mru buffer that need to be updated
 * @param cpu_id current cpu id
 * @param mru_size mru buffer size
 *
 * Flush the per vcpu mru_buf
 * If the length of MRU buffer is over the size of the buffer,
 * it reinitializes the MRU buffer to new entries
 */
static void flush_active_mru(struct emp_mm *emm, struct slru *target_list,
		struct emp_list *mru_list, int cpu_id, const int mru_size)
{
	struct list_head *cur, *n;
	int mru_list_len;
	struct local_page *lp;
	struct temp_list to_global;
	struct emp_list *global_list = &target_list->list;

	init_temp_list(&to_global);

	emp_list_lock(mru_list);
	emp_list_for_each_safe(cur, n, mru_list) {
		lp = __get_local_page_from_list(cur);
		debug_assert(is_local_page_on_mru(lp));
		if (emp_trylock_local_page(emm, lp)) {
			emp_list_del(cur, mru_list);
			temp_list_add_tail(cur, &to_global);
		}
	}
	mru_list_len = emp_list_len(mru_list);

	if (mru_list_len > (mru_size * 2)) {
		emp_list_unlock(mru_list);
		emp_list_lock(global_list);
	} else if (emp_list_trylock(global_list)) {
		emp_list_unlock(mru_list);
	} else {
		/* re-insertion */
		temp_list_for_each_safe(cur, n, &to_global) {
			lp = __get_local_page_from_list(cur);
			temp_list_del(cur, &to_global);
			emp_list_add_tail(cur, mru_list);
			emp_unlock_local_page(emm, lp);
		}
		emp_list_unlock(mru_list);
		return;
	}

	temp_list_for_each_safe(cur, n, &to_global) {
		lp = __get_local_page_from_list(cur);
		set_local_page_global(lp);
		temp_list_del(cur, &to_global);
		emp_list_add_tail(cur, global_list);
		emp_unlock_local_page(emm, lp);
	}
	emp_list_unlock(global_list);
}

/**
 * add_gpas_to_active_list - Add given gpas to the active list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param gpas gpas to add
 * @param n_new the number of gpas
 *
 * @return the length of new pages
 *
 * I think that this function should be separated for higher performance
 */
int add_gpas_to_active_list(struct emp_mm *bvma, struct vcpu_var *cpu, 
			    struct emp_gpa **gpas, int n_new)
{
	int new_pages_len = 0;
	struct emp_vmr *vmr;
	/* proactive list or active list */
	struct slru *slru;
	struct emp_list *list;
	struct temp_list temp;
	int i, cpu_id;

	debug_add_gpas_to_active_list(gpas, n_new);

	slru = &bvma->ftm.active_list;

	cpu_id = cpu->id;
	init_temp_list(&temp);


	for (i = 0; i < n_new; i++) {
		struct emp_gpa *head;
		struct emp_gpa *gpa = gpas[i];
		vmr = bvma->vmrs[gpa->local_page->vmr_id];

		head = emp_get_block_head(gpa);
		if (!is_unmapped_active(head))
			head->r_state = GPA_ACTIVE;

		set_local_page_cpu_mru(head->local_page, cpu_id);
		temp_list_add_tail(&head->local_page->lru_list, &temp);
		new_pages_len += gpa_block_size(head);
	}

	list = get_list_ptr_mru(bvma, slru, cpu_id);
	emp_list_lock(list);
	emp_list_splice_tail(&temp, list);
	emp_list_unlock(list);

	if (MRU_BUF_FULL(MRU_QUEUE_DEPTH, emp_list_len(list)))
		flush_active_mru(bvma, slru, list, cpu_id, MRU_QUEUE_DEPTH);

	if (new_pages_len)
		atomic_add(new_pages_len, &slru->page_len);

	return new_pages_len;
}

static inline bool COMPILER_DEBUG
gpa_acquire(struct emp_vmr *vmr, struct emp_gpa *head)
{
	struct emp_gpa *g;
	struct page *p;
	int page_size, ret;
	int count_max, count_private;
	int count_shared = emp_lp_count_pmd(head->local_page);

#ifdef CONFIG_EMP_VM
	if (unlikely(is_gpa_flags_set(head, GPA_LOWMEM_BLOCK_MASK)))
		return false;
#endif

	if (!is_gpa_flags_set(head, GPA_HPT_MASK))
		return true;

	ret = true;
	count_shared = emp_lp_count_pmd(head->local_page);
	for_each_gpas(g, head) {
		p = g->local_page->page;
		page_size = __local_gpa_to_page_len(vmr, g);

		count_max = emp_page_count_max(p, page_size);
		count_private = 1 + (PageCompound(p)?page_size:1)*count_shared;

		if (count_max > count_private) {
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
			int refcnt, mapcnt;
			if (!is_debug_page_ref_correct(g->local_page, &refcnt, &mapcnt, 0)) {
				struct local_page *lp = g->local_page;
				emp_debug_bulk_msg_lock();
				printk(KERN_ERR "DEBUG: [CANNOT_ACQUIRE] (%s) lp: %016lx vmr: %d gpa: %lx curr: %2d sum: %2d map: %2d mmu: %d io: %d pte: %d unmap: %d counnt_max: %d count_private: %d\n",
						__func__,
						(unsigned long) lp,
						lp->vmr_id,
						lp->gpa_index,
						refcnt,
						lp->debug_page_ref_sum,
						mapcnt,
						lp->debug_page_ref_in_mmu_noti,
						lp->debug_page_ref_in_io,
						lp->debug_page_ref_in_will_pte,
						lp->debug_page_ref_in_unmap,
						count_max, count_private);
				__debug_page_ref_print_all(lp);
				emp_debug_bulk_msg_unlock();
			}
#endif
			ret = false;
			break;
		}
	}

	return ret;
}

#ifdef CONFIG_EMP_BLOCK
bool __wait_for_prefetched_block(struct emp_mm *emm, struct vcpu_var *cpu, 
				 struct emp_gpa *head)
{
	struct emp_gpa *g, *pf_sb_head;
	bool cpf_prefetched;
	int pf_sb_base, pf_sb_offset;
	int demand_offset;

	/* release work requests of prefetch sub-blocks */
	if (!clear_gpa_flags_if_set(head, GPA_PREFETCHED_MASK))
		return false;

	debug_wait_for_prefetch_subblocks(head);

	demand_offset = head->local_page->demand_offset;
	pf_sb_base = demand_offset >> gpa_subblock_order(head);
	pf_sb_offset = demand_offset & gpa_subblock_mask(head);
	pf_sb_head = head + pf_sb_base;

	cpf_prefetched = is_gpa_flags_set(head, GPA_PREFETCHED_CPF_MASK);
	for_each_gpas(g, head) {
		if (g == pf_sb_head && !cpf_prefetched)
			continue;

		if (!emm->sops.wait_read_async(emm, cpu, g))
			continue;

		clear_gpa_flags_if_set(g, GPA_REMOTE_MASK);

#ifdef CONFIG_EMP_VM
		/* To ensure the page reference count at unmap_gpas()
		 * HPT: 2
		 * EPT: 1
		 * TODO: Actually, reference counter of prefetched HPT
		 * should be reduced */
		if (!is_gpa_flags_set(head, GPA_EPT_MASK))
			continue;
		if (g == pf_sb_head && cpf_prefetched)
			emp_put_subblock_except(g, pf_sb_offset);
		else
			emp_put_subblock(g);
#else
		//continue; // This is unnecessary for now.
#endif
	}
	head->local_page->demand_offset = 0;
	return true;
}

/**
 * wait_for_prefetch_subblocks - Wait for the fetching prefetch sub-blocks
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param vs victims list
 * @param n_vs the number of victims
 *
 * Since the blocks in the inactive list are unmapped,
 * all the prefetch work requests relatd in the block should be done
 * before the block is moved on the inactive list.
 */
void wait_for_prefetch_subblocks(struct emp_mm *emm, struct vcpu_var *cpu,
				 struct emp_gpa *vs[], int n_vs)
{
	int i;
	for (i = 0; i < n_vs; i++) {
		if (__wait_for_prefetched_block(emm, cpu, vs[i]) == false)
			continue;
	}
}
EXPORT_SYMBOL(wait_for_prefetch_subblocks);
#endif

/**
 * select_victims_al - select victims from active list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param vs victims list
 * @param vs_len the number of victims in a list
 *
 * @return the number of victims
 */
static int COMPILER_DEBUG
select_victims_active_list(struct emp_mm *bvma, struct vcpu_var *cpu,
				struct emp_gpa **vs, int vs_len)
{
	struct emp_list *list;
	struct list_head *cur, *n;
	int cpu_id;
	int retry = 0;

	int n_vs = 0, vs_pages_len = 0;

	struct slru *target_list = &bvma->ftm.active_list;

	cpu_id = cpu->id;
	list = get_list_ptr_lru(bvma, target_list, cpu_id);

retry_start:
	emp_list_lock(list);

	/* select vs from active list */
	emp_list_for_each_safe(cur, n, list) {
		struct emp_gpa *v;
		struct local_page *lp;

		lp = __get_local_page_from_list(cur);
		debug_assert(get_local_page_cpu(lp) == cpu_id
					&& is_local_page_on_lru(lp));

		if ((v = emp_trylock_local_page(bvma, lp)) == NULL)
			continue;

		if (!is_unmapped_active(v) &&
				gpa_acquire(bvma->vmrs[lp->vmr_id], v)) {
			emp_list_del(&lp->lru_list, list);
			v->r_state = GPA_TRANS_AL;
			vs[n_vs++] = v;
			vs_pages_len += gpa_block_size(v);
			if ((n_vs >= vs_len) ||
				(n_vs >= NUM_VICTIM_CLUSTER))
				break;
			continue;
		}

		emp_unlock_block(v);
	}

	emp_list_unlock(list);

	if (n_vs)
		atomic_sub(vs_pages_len, &target_list->page_len);
	else if  (emp_list_len(list) < LRU_QUEUE_DEPTH) {
		int refill = refill_lru_buf(bvma, target_list, list, cpu_id,
							LRU_QUEUE_DEPTH);
		// one time retry with refilled lru list
		if (refill && retry == 0) {
			retry = 1;
			goto retry_start;
		}
	}

	return n_vs;
}


/**
 * update_active_list - select victims from active list and reclaim
 * @param emm emm data structure
 * @param local_cpu working vcpu ID
 * @param vs victims list
 * @param vs_max the maximum number of victims
 *
 * @return the number of victims
 *
 * Select victims and reclaim the victims with moving it to the inactive list \n
 * If there are remained victims, the victims are rolled back to the active list
 */
static int 
update_active_list(struct emp_mm *emm, struct vcpu_var *local_cpu,
		   struct emp_gpa **vs, int vs_max)
{
	int n_vs = 0;
	struct vcpu_var *cpu;
	int cpu_id;
	struct emp_gpa *new_vs[vs_max];
	struct emp_gpa *active_gpas[vs_max];
	int active_gpas_len = 0;
	int i, n_new_victim = 0;

	for_all_vcpus_from(cpu, cpu_id, local_cpu, emm) {
		n_new_victim += select_victims_active_list(emm, cpu,
				new_vs + n_new_victim,
				vs_max - n_new_victim);
		if (n_new_victim >= vs_max)
			break;
	}

	if (unlikely(n_new_victim == 0))
		return 0;

	/* release work requests of prefetch sub-blocks */
	wait_for_prefetch_subblocks(emm, local_cpu, new_vs, n_new_victim);

	/* reclaiming gpas to move them to inactive list */
	n_vs = reclaim_gpa_many(emm, new_vs, n_new_victim, vs, active_gpas);
	if (n_vs == n_new_victim)
		goto out;

	/* failed to reclaim a few gpas. */
	active_gpas_len = n_new_victim - n_vs;
	add_gpas_to_active_list(emm, local_cpu, active_gpas, active_gpas_len);
	for (i = 0; i < active_gpas_len; i++)
		emp_unlock_block(active_gpas[i]);
out:
	return n_vs;
}

/**
 * check_block_free - Check if the block will be freed
 * @param bvma bvma data structure
 * @param head head of the block
 *
 * @retval true: Not free
 * @retval false: Free
 *
 * Currently this block is writebacked to secondary memory. \n
 * So, the block will be freed.
 */
static bool check_block_free(struct emp_mm *bvma, struct emp_gpa *head)
{
	struct emp_gpa *g;
	struct page *p;
	int subblock_size = gpa_subblock_size(head);
	bool not_free = false;

	for_each_gpas(g, head) {
		p = g->local_page->page;
		if (emp_page_count_max(p, subblock_size) != 1) {
			not_free = true;
			break;
		}
	}

	return !not_free;
}

/**
 * select_victims_inactive_list - select victims from inactive list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param vs victims list
 * @param vs_len the number of victims in a list
 * @param ctime current time
 * @param need_tlb_flush
 *
 * @return the number of victims
 */
static int select_victims_inactive_list(struct emp_mm *bvma,
					struct vcpu_var *cpu,
					struct emp_gpa **vs, int vs_len)
{
	struct emp_list *list;
	struct list_head *cur, *n;
	struct slru *target_list;
	int n_vs = 0, cpu_id;

	/* retrieve inactive list info */
	target_list = &bvma->ftm.inactive_list;
	cpu_id = cpu->id;
	list = get_list_ptr_inactive(bvma, target_list, cpu_id);

	/* select vs from inactive list */
	emp_list_lock(list);

	emp_list_for_each_safe(cur, n, list) {
		struct local_page *lp;
		bool reinsert_gpa;
		struct emp_gpa *v;

		lp = __get_local_page_from_list(cur);
		debug_assert(get_local_page_cpu(lp) == cpu_id && is_local_page_on_lru(lp));

		if ((v = emp_trylock_local_page(bvma, lp)) == NULL)
			continue;

		reinsert_gpa = !check_block_free(bvma, v);

		// inactive_list.len will be decreased when GPA_WB->GPA_INIT
		// by calling sub_inactive_list_page_len
		if (reinsert_gpa == false) {
			emp_list_del(&lp->lru_list, list);
			debug_check_notlocked(v);

			v->r_state = GPA_TRANS_IL;
			vs[n_vs++] = v;
			if (n_vs >= vs_len)
				break;
			continue;
		}

		emp_unlock_block(v);
	}

	emp_list_unlock(list);
	return n_vs;
}

/**
 * evict_block - Evict victims from local memory to donor
 * @param emm emm data structure
 * @param cpu working vcpu ID
 * @param head head of the evicted block
 * @param eager_evict is eager_evict?
 *
 * @return work request of head
 */
static struct work_request *
evict_block(struct emp_mm *emm, struct vcpu_var *cpu, struct emp_gpa *head,
		bool eager_evict)
{
	struct emp_gpa *victim = NULL;
	int ew_len;
	unsigned int dma_size;
	struct work_request *head_wr, *tail_wr, *eh_wr, *w;

	dma_size = min((unsigned int)gpa_block_size(head),
			(unsigned int)emm->mrs.memregs_wdma_size);
	dma_size = min((u8)dma_size, (u8)(bvma_subblock_size(emm)));

	head_wr = NULL;
	tail_wr = NULL;
	eh_wr = NULL;

	debug_evict_block(head);

	if (!alloc_remote_page(emm, head))
		goto error;

	ew_len = 0;
	for_each_gpas(victim, head) {
		debug_page_ref_io_beg(victim->local_page);
		emp_get_subblock(victim, false);
		emp_lock_subblock(victim);

		/* this code block writes gpas to remote device at smaller size */
		w = emm->sops.post_writeback_async(emm, victim, cpu,
						head_wr, tail_wr, dma_size,
						eager_evict ? false : true);
		if (unlikely(w == NULL))
			goto error;

		if (w->chained_ops) {
			link_entangled_writeback(w, head_wr, &tail_wr,
						 &ew_len);
		} else if (head_wr) {
			list_add_tail(&w->subsibling, &head_wr->subsibling);
		}
		if (!head_wr)
			head_wr = w;
	}

	if (head_wr && head_wr->chained_ops) {
		struct dma_ops *ops = head_wr->dma_ops;
		ops->post_work_req(head_wr, tail_wr, head_wr, ew_len);
	}

	if (!eager_evict) {
		int cpu_id = emm->sops.push_writeback_request(emm, head_wr, cpu);
		head->r_state = GPA_WB;
		set_local_page_cpu_lru(head->local_page, cpu_id);
	}

	return head_wr;

error:
	if (head_wr == NULL) {
		if (victim != NULL) {
			emp_unlock_subblock(head);
			debug_page_ref_io_end(head->local_page);
			emp_put_subblock(head);
		}
	} else {
		struct emp_gpa *gpa;
		while (list_empty(&head_wr->subsibling)) {
			w = list_last_entry(&head_wr->subsibling,
						struct work_request,
						subsibling);
			list_del_init(&w->remote_pages);
			wait_write(emm, w, cpu, false);
			free_work_request(emm, w);
		}

		for_each_gpas(gpa, head) {
			if (gpa > victim)
				break;
			emp_unlock_subblock(gpa);
			debug_page_ref_io_end(gpa->local_page);
			emp_put_subblock(gpa);
		}
	}

	for_each_gpas(victim, head) {
		if (is_gpa_remote_page_free(victim))
			continue;
		free_remote_page(emm, victim, true);
	}

	return ERR_PTR(-ENXIO);
}

/**
 * emp_writeback_block - Writeback blocks to donor if needed
 * @param bvma bvma data structure
 * @param head head of the evicted block
 * @param cpu working vcpu ID
 *
 * @retval 1: Writeback is done
 * @retval 0: No writeback
 * @retval -1: error occurs
 */
int emp_writeback_block(struct emp_mm *emm, struct emp_gpa *head,
				struct vcpu_var *cpu)
{
	return PTR_ERR_RET(evict_block(emm, cpu, head, false));
}

/**
 * update_inactive_list - select victims from inactive list and writeback
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param pressure number of writeback
 *
 * @return the number of victims
 *
 * Select victims in inactive list and writeback the victims to donor
 */
static int update_inactive_list(struct emp_mm *bvma, struct vcpu_var *local_cpu,
				int pressure)
{
	struct emp_gpa *victims[pressure], **vs;
	struct vcpu_var *cpu;
	int cpu_id;
	struct emp_list *list;
	int n_victims, n_victim_pages, n_vs;
	int i;
	struct slru *inactive_list = &bvma->ftm.inactive_list;

	n_victims = n_victim_pages = 0;

	for_all_vcpus_from(cpu, cpu_id, local_cpu, bvma) {
		int c = cpu->id;
		list = get_list_ptr_inactive(bvma, inactive_list, c);
		if (emp_list_len(list) == 0)
			continue;
		vs = victims + n_victims;
		n_vs = pressure - n_victims;
		n_victims += select_victims_inactive_list(bvma, cpu, vs, n_vs);
		if (n_victims >= pressure)
			break;
	}

	for (i = 0; i < n_victims; i++) {
		// head of victims[i] is locked at select_victims_inactive_list
		int writeback_done;
		struct emp_gpa *head = victims[i];

		debug_update_inactive_list(head,
				emp_get_block_head(victims[i]));

		debug_update_inactive_list2(head);

		n_victim_pages += gpa_block_size(head);
		writeback_done = emp_writeback_block(bvma, head, local_cpu);
		if (unlikely(writeback_done < 0)) {
			n_victim_pages = writeback_done; /* error code */
			goto error;
		}
		emp_unlock_block(head);
	}

#ifdef CONFIG_EMP_BLOCKDEV
	if (bvma->mrs.blockdev_used)
		io_schedule();
#endif

	return n_victim_pages;

error:
	for ( ; i < n_victims; i++)
		emp_unlock_block(victims[i]);
#ifdef CONFIG_EMP_BLOCKDEV
	if (bvma->mrs.blockdev_used)
		io_schedule();
#endif
	return n_victim_pages;
}

/**
 * add_gpas_to_inactive - Add blocks to inactive list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param gpas blocks to insert into the inactive list
 * @param n_new the number of pages of the blocks
 *
 * Add blocks to inactive list and check unmapped dirty blocks to writeback
 */
int add_gpas_to_inactive(struct emp_mm *bvma, struct vcpu_var *cpu,
				struct emp_gpa **gpas, int n_new)
{
	int new_pages_len = 0;

	/* inactive list */
	struct slru *slru;
	struct emp_list *list;
	int i, cpu_id;
	struct emp_gpa *head;

	debug_add_gpas_to_inactive(gpas, n_new);

	slru = &bvma->ftm.inactive_list;
	cpu_id = cpu->id;
	list = get_list_ptr_inactive(bvma, slru, cpu_id);

	emp_list_lock(list);
	// add blocks to inactive list
	for (i = 0; i < n_new; i++) {
		head = gpas[i];
		debug_add_gpas_to_inactive2(head,
				emp_get_block_head(gpas[i]));

		head->r_state = GPA_INACTIVE;
		set_local_page_cpu_lru(head->local_page, cpu_id);
		emp_list_add_tail(&head->local_page->lru_list, list);
		debug_add_inactive_list_page_len(bvma, head);
		new_pages_len += gpa_block_size(head);
	}
	emp_list_unlock(list);

	if (new_pages_len)
		__add_inactive_list_page_len(new_pages_len, bvma);

	return new_pages_len;
}

/**
 * _update_lru_lists - Update LRU lists
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 * @param ps_force pressure
 * @param verbose need to show debug msg?
 *
 * Check the number of pages in LRU lists and move the pages among the lists
 * if the lists are full
 */
static int _update_lru_lists(struct emp_mm *bvma, struct vcpu_var *cpu,
				int ps_force, int verbose)
{
	int i, ret = 0;
	int n_victim = 0, n_unmap_pages, n_released_pages;
	struct emp_gpa *victims[NUM_VICTIM_CLUSTER];

	// for active queue
	n_unmap_pages = 0;
	if (is_active_list_full(bvma)) {
		n_victim = update_active_list(bvma, cpu, victims,
				NUM_VICTIM_CLUSTER);
		if (n_victim > 0) {
			ret = add_gpas_to_inactive(bvma, cpu, victims, n_victim);
			if (likely(ret > 0))
				n_unmap_pages += ret;
			for (i = 0; i < n_victim; i++)
				emp_unlock_block(victims[i]);
			if (unlikely(ret < 0))
				return ret;
		}
	}

	// for inactive queue
	n_released_pages = 0;
	n_unmap_pages = max(n_unmap_pages, ps_force);
	if (is_inactive_list_full(bvma, n_unmap_pages)) {
		emp_wait_for_writeback(bvma, cpu, 
				NUM_VICTIM_CLUSTER << bvma_block_order(bvma));
		ret = update_inactive_list(bvma, cpu, NUM_VICTIM_CLUSTER);
		if (unlikely(ret < 0))
			return ret;
		n_released_pages = ret;
	}

	if (verbose) {
		printk(KERN_DEBUG 
			"cpu: %d n_victim: %d n_unmape_pages: %d n_released_pages: %d\n",
			cpu->id, n_victim, n_unmap_pages, n_released_pages);
	}

	return ret;
}

/**
 * update_lru_lists_reref - Update LRU lists when the blocks are re-referenced
 * @param b bvma data structure
 * @param v working vcpu ID
 * @param new_gs new blocks
 * @param n_new_gs the number of new blocks
 * @param pressure block size
 *
 * When the blocks are re-referenced, it inserts blocks in a MRU order
 * (from active list to inactive list)
 */
/* functions to update lru lists of gpas */
int update_lru_lists_reref(struct emp_mm *b, struct vcpu_var *cpu,
			   struct emp_gpa **new_gs, int n_new_gs,
			   const int pressure)
{
	if (n_new_gs)
		add_gpas_to_active_list(b, cpu, new_gs, n_new_gs);
	return _update_lru_lists(b, cpu, pressure, 0);
}

/**
 * update_lru_lists - Update LRU lists when the blocks are fetched to local memory
 * @param b bvma data structure
 * @param v working vcpu ID
 * @param new_gs new blocks
 * @param n_new_gs the number of new blocks
 * @param pressure block size
 *
 * It selects and evicts a victim by updating LRU lists. \n
 * When the block is inserted to the lists,
 */
int update_lru_lists(struct emp_mm *b, struct vcpu_var *cpu,
		     struct emp_gpa **new_gs, int n_new_gs, const int pressure)
{
	if (n_new_gs)
		add_gpas_to_active_list(b, cpu, new_gs, n_new_gs);
	return _update_lru_lists(b, cpu, pressure, 0);
}

/**
 * update_lru_lists_lru - Insert the fetched block in a LRU order
 * @param b bvma data structure
 * @param v working vcpu ID
 * @param new_gs new blocks
 * @param n_new_gs the number of blocks
 * @param pressure block size
 *
 * @return the number of victim from the inactive list
 *
 * It inserts blocks to inactive list
 */
int update_lru_lists_lru(struct emp_mm *b, struct vcpu_var *cpu,
			 struct emp_gpa **new_gs, int n_new_gs, int pressure)
{
	int r, ret;

	if (n_new_gs) {
		r = add_gpas_to_inactive(b, cpu, new_gs, n_new_gs);
		if (unlikely(r < 0))
			return r;
	}

	ret = 0;
	if (is_inactive_list_full(b, pressure)) {
		r = update_inactive_list(b, cpu, NUM_VICTIM_CLUSTER);
		if (unlikely(r < 0))
			return r;
		ret += r;
	}
	return ret;
}

/**
 * _reclaim_gpa_many - Reclaim mutliple gpas
 * @param bvma bvma data structure
 * @param gpas gpas to reclaim
 * @param n_gpas the number of gpas
 * @param tlb_flush_force should do TLB flush?
*/
static int 
_reclaim_gpa_many(struct emp_mm *bvma, struct emp_gpa *gpas[], int n_gpas,
		  bool *tlb_flush_force)
{
	int i, end;

	if (unlikely(n_gpas <= 0))
		return -1;

	end = n_gpas - 1; // always larger than or equal to 0
	for (i = 0; i < end; i++)
		bvma->vops.unmap_gpas(bvma, gpas[i], tlb_flush_force);

	*tlb_flush_force = true;
	bvma->vops.unmap_gpas(bvma, gpas[end], tlb_flush_force);


	return 0;
}

/**
 * check_gpa_block_reclaimable - Check if the block is reclaimable
 * @param bvma bvma data structure
 * @param gpa gpa of the block
 *
 * @retval true: Reclaimable
 * @retval false: Not reclaimable
 */
bool check_gpa_block_reclaimable(struct emp_mm *bvma, struct emp_gpa *gpa)
{
	struct emp_vmr *vmr;
	struct emp_gpa *head, *g;
	struct page *page;
	int expected_page_count;
	int page_size, shared_count;

	vmr = bvma->vmrs[gpa->local_page->vmr_id];
	head = emp_get_block_head(gpa);

	/* if flags are are unset -> skip this gpa */
	if (is_gpa_flags_same(head, GPA_nPT_MASK, 0))
		return true;

	shared_count = page_mapcount(head->local_page->page);
	for_each_gpas(g, head) {
		/* Normally, page reference count is increased.
		 * However, when mapped to EPT, page reference count is not increased.
		 *
		 * If the gpa is mapped to HPT (for I/O), page count should be two.
		 * While serving I/O operations, page count can be more than two.
		 * Those pages should not be reclaimed.
		 */
		/* code block not to reclaim I/O pages */
		expected_page_count = 1;

		page = g->local_page->page;
		page_size = __local_gpa_to_page_len(vmr, g);
		if (is_gpa_flags_set(head, GPA_HPT_MASK))
			expected_page_count += shared_count *
				(PageCompound(page)? page_size: 1);

		if (emp_page_count_max(page, page_size) >
				expected_page_count)
			return false;
	}
	return true;
}
EXPORT_SYMBOL(check_gpa_block_reclaimable);

static int _reclaim_gpa(struct emp_mm *bvma, struct emp_gpa *gpa,
			bool *tlb_flush_force)
{
	_reclaim_gpa_many(bvma, &gpa, 1, tlb_flush_force);
	return 0;
}

int reclaim_gpa(struct emp_mm *bvma, struct emp_gpa *gpa, bool *tlb_flush_force)
{
	if (!check_gpa_block_reclaimable(bvma, gpa))
		return 1;
	return _reclaim_gpa(bvma, gpa, tlb_flush_force);
}
EXPORT_SYMBOL(reclaim_gpa);

/**
 * reclaim_gpa_many - Separate active blocks and victim blocks, and reclaim the victim blocks
 * @param bvma bvma data structure
 * @param gpas gpas to reclaim
 * @param n_gpas the number of gpas
 * @param victims victim blocks list
 * @param actives active blocks list
 *
 * @return the number of victims
*/
static int
reclaim_gpa_many(struct emp_mm *bvma, struct emp_gpa **gpas, int n_gpas,
		struct emp_gpa **victims, struct emp_gpa **actives)
{
	int n_victims = 0, n_actives = 0, i;
	bool tlb_flush_force = false;

	/* select active gpas and victim gpas */
	for (i = 0; i < n_gpas; i++) {
		if (!check_gpa_block_reclaimable(bvma, gpas[i])) {
			actives[n_actives++] = gpas[i];
		} else {
			victims[n_victims++] = gpas[i];
		}
	}

	if (n_victims)
		_reclaim_gpa_many(bvma, victims, n_victims, &tlb_flush_force);

	return n_victims;
}

/**
 * reclaim_emp_pages - Reclaim a specific number of blocks
 * @param b bvma data structure
 * @param cpu working vcpu ID
 * @param pressure the number of pages to reclaim
 * @param force force reclaim?
 *
 * @return the number of reclaimed pages
*/
int reclaim_emp_pages(struct emp_mm *b, struct vcpu_var *cpu, int pressure,
				bool force)
{
	int reclaimed_pages = 0;
#ifdef CONFIG_EMP_BLOCK
	int pressure_block = min(pressure >> b->config.block_order,
				 NUM_VICTIM_CLUSTER);
#else
	int pressure_block = min(pressure, NUM_VICTIM_CLUSTER);
#endif

	if (force && VCPU_WB_REQUEST_EMPTY(cpu)) {
		reclaimed_pages = update_inactive_list(b, cpu, pressure_block);
		if (unlikely(reclaimed_pages < 0))
			return reclaimed_pages;
	}

	if (is_active_list_full(b) ||
			is_inactive_list_full(b, pressure)) {
		int ret = _update_lru_lists(b, cpu, pressure, 0);
		if (unlikely(ret < 0))
			return ret;
	}

	return reclaimed_pages;
}

static inline void
__remove_from_active_list(struct emp_mm *emm, struct emp_gpa *gpa) {
	struct slru *active = &emm->ftm.active_list;
	struct emp_list *list;
	struct local_page *lp = gpa->local_page;

	debug_assert(gpa->r_state == GPA_ACTIVE);
	debug_assert(lp);
	debug_assert(is_local_page_on_list(lp));
	debug_assert(!list_empty(&lp->lru_list));
	debug_assert(lp->lru_list.next != LIST_POISON1 && lp->lru_list.prev != LIST_POISON2);

	if (is_local_page_on_mru(lp)) {
		list = get_list_ptr_mru(emm, active, lp->cpu);
	} else if (is_local_page_on_lru(lp)) {
		list = get_list_ptr_lru(emm, active, lp->cpu);
	} else {
		debug_assert(is_local_page_on_global(lp));
		list = &active->list;
	}

	emp_list_lock(list);
	emp_list_del(&gpa->local_page->lru_list, list);
	emp_list_unlock(list);
	atomic_sub(gpa_block_size(gpa), &active->page_len);
	clear_local_page_list_flags(gpa->local_page);
}

static inline void
__remove_from_inactive_list(struct emp_mm *emm, struct emp_gpa *gpa) {
	struct slru *inactive = &emm->ftm.inactive_list;
	struct emp_list *list;
	struct local_page *lp = gpa->local_page;

	debug_assert(gpa->r_state == GPA_INACTIVE);
	debug_assert(lp);
	if (!is_local_page_on_list(lp))
		return;

	debug_assert(!list_empty(&lp->lru_list));
	debug_assert(lp->lru_list.next != LIST_POISON1 && lp->lru_list.prev != LIST_POISON2);

	list = get_list_ptr_inactive(emm, inactive, lp->cpu);

	emp_list_lock(list);
	emp_list_del(&gpa->local_page->lru_list, list);
	emp_list_unlock(list);
	sub_inactive_list_page_len(emm, gpa);
	clear_local_page_list_flags(gpa->local_page);
}

void remove_gpa_from_lru(struct emp_mm *emm, struct emp_gpa *head)
{
	if (head->r_state == GPA_ACTIVE)
		__remove_from_active_list(emm, head);
	else if (head->r_state == GPA_INACTIVE)
		__remove_from_inactive_list(emm, head);
}

/**
 * reclaim_set - Set parameters for lru chain
 * @param emm emm data structure
 *
 * Set paramters for lru chain
*/
void reclaim_set(struct emp_mm *emm)
{
	struct emp_ftm *f = &emm->ftm;
	/* initialize emm */
	f->inactive_pages_len = atomic_read(&f->local_cache_pages) >> 3;
	f->active_pages_len = ((atomic_read(&f->local_cache_pages)
					- f->inactive_pages_len));
}

/**
 * reclaim_init - Initialize LRU lists
 * @param bvma bvma data structure
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * EMP maintains 2 lists
 * + Active list: active pages
 * + Inactive list: the page is located in local memory but the mapping info is disabled (unmapped)
*/
int reclaim_init(struct emp_mm *bvma)
{
	int cpu;
#ifdef CONFIG_EMP_VM
	int i, vcpu_len = EMP_KVM_VCPU_LEN(bvma);
#endif
	struct slru *active, *inactive;
	struct emp_list *mru, *lru;

	active = &bvma->ftm.active_list;
	inactive = &bvma->ftm.inactive_list;

	/* initialize global data */
	reclaim_set(bvma);
	init_emp_list(&active->list);
	init_emp_list(&inactive->list);
#ifdef CONFIG_EMP_VM
#ifdef CONFIG_EMP_DEBUG
	active->abuf_len = vcpu_len;
	inactive->abuf_len = vcpu_len;
#endif /* CONFIG_EMP_DEBUG */

	/* initialize per-vcpu active_list */
	/* note: mru_bufs[0 ~ (#vcpu)] is initialized.
	 * mru_bufs[0] will not be used because we do not use cpu_id 0 
	 * lru_bufs[0] is the same */
	active->mru_bufs = emp_kmalloc(sizeof(struct emp_list) * vcpu_len, GFP_KERNEL);
	active->lru_bufs = emp_kmalloc(sizeof(struct emp_list) * vcpu_len, GFP_KERNEL);
	if (!active->mru_bufs || !active->lru_bufs)
		goto reclaim_init_fail;

	for (i = 0; i < vcpu_len; i++) {
		mru = &active->mru_bufs[i];
		init_emp_list(mru);

		lru = &active->lru_bufs[i];
		init_emp_list(lru);
	}

	/* initialize per-vcpu inactive_list */
	inactive->sync_bufs = emp_kmalloc(sizeof(struct emp_list) * vcpu_len, GFP_KERNEL);
	if (!inactive->sync_bufs)
		goto reclaim_init_fail;

	for (i = 0; i < vcpu_len; i++) {
		init_emp_list(&inactive->sync_bufs[i]);
	}
#endif /* CONFIG_EMP_VM */


	/* initialize per-pcpu active_list */
	active->host_lru = emp_alloc_pcdata(struct emp_list);
	active->host_mru = emp_alloc_pcdata(struct emp_list);
	if (active->host_lru == NULL || active->host_mru == NULL)
		goto reclaim_init_fail;

	for_each_possible_cpu(cpu) {
		mru = emp_pc_ptr(active->host_mru, cpu);
		init_emp_list(mru);
		lru = emp_pc_ptr(active->host_lru, cpu);
		init_emp_list(lru);
	}

	/* initialize per-pcpu inactive_list */
	inactive->host_lru = emp_alloc_pcdata(struct emp_list);
	if (inactive->host_lru == NULL)
		goto reclaim_init_fail;

	for_each_possible_cpu(cpu) {
		lru = emp_pc_ptr(inactive->host_lru, cpu);
		init_emp_list(lru);
	}

	return 0;

reclaim_init_fail:
#ifdef CONFIG_EMP_VM
	if (inactive->sync_bufs) {
		emp_kfree(inactive->sync_bufs);
		inactive->sync_bufs = NULL;
	}
	if (active->mru_bufs) {
		emp_kfree(active->mru_bufs);
		active->mru_bufs = NULL;
	}
	if (active->lru_bufs) {
		emp_kfree(active->lru_bufs);
		active->lru_bufs = NULL;
	}
#endif /* CONFIG_EMP_VM */
	if ((inactive->host_lru)) {
		emp_free_pcdata(inactive->host_lru);
		inactive->host_lru = NULL;
	}
	if ((active->host_lru)) {
		emp_free_pcdata(active->host_lru);
		active->host_lru = NULL;
	}
	if ((active->host_mru)) {
		emp_free_pcdata(active->host_mru);
		active->host_mru = NULL;
	}
#ifdef CONFIG_EMP_VM
#ifdef CONFIG_EMP_DEBUG
	active->abuf_len = 0;
	inactive->abuf_len = 0;
#endif
#endif /* CONFIG_EMP_VM */

	return -ENOMEM;
}

/**
 * reclaim_exit - Release LRU lists
 * @param emm emm data structure
 *
 * EMP maintains 2 lists
 * + Active list: active pages
 * + Inactive list: the page is located in local memory but the mapping info is disabled (unmapped)
*/
void reclaim_exit(struct emp_mm *emm)
{
	struct slru *active = &emm->ftm.active_list;
	struct slru *inactive = &emm->ftm.inactive_list;

	debug_reclaim_exit_active(emm, active);
	debug_reclaim_exit_inactive(emm, inactive);

#ifdef CONFIG_EMP_VM
	if (inactive->sync_bufs) {
		emp_kfree(inactive->sync_bufs);
		inactive->sync_bufs = NULL;
	}
	if (active->mru_bufs) {
		emp_kfree(active->mru_bufs);
		active->mru_bufs = NULL;
	}
	if (active->lru_bufs) {
		emp_kfree(active->lru_bufs);
		active->lru_bufs = NULL;
	}
#endif /* CONFIG_EMP_VM */

	if (inactive->host_lru) {
		emp_free_pcdata(inactive->host_lru);
		inactive->host_lru = NULL;
	}
	if (active->host_lru) {
		emp_free_pcdata(active->host_lru);
		active->host_lru = NULL;
	}
	if (active->host_mru) {
		emp_free_pcdata(active->host_mru);
		active->host_mru = NULL;
	}
#ifdef CONFIG_EMP_VM
#ifdef CONFIG_EMP_DEBUG
	active->abuf_len = 0;
	inactive->abuf_len = 0;
#endif /* CONFIG_EMP_DEBUG */
#endif /* CONFIG_EMP_VM */
}

