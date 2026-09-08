#include <linux/kthread.h>
#include <linux/wait.h>
#include <asm/fpu/api.h>
#include "vm.h"
#include "gpa.h"
#include "alloc.h"
#include "reclaim.h"
#include "els_block.h"
#include "block-flag.h"
#include "donor_mem_rw.h"
#include "debug.h"
#include "vcpu_var.h"
#ifdef CONFIG_EMP_USER
#include "cow.h"
#endif
#include "pcalloc.h"

// we assume that TLB valid period without TLB shootdown is 10ms
#define TLB_VALID_PERIOD (10ULL * MS_TO_NS)
#define MRU_BUF_FULL(size, len) ((len) >= (size))

#ifdef CONFIG_EMP_VM
static void __noop(void *dummy) {}

/**
 * tlb_flush_all - Make all the CPUs to flush tlb
 * @param kvm kvm info
 */
static void tlb_flush_all(struct kvm *kvm)
{
	unsigned long i;
	int cpu, me;
	cpumask_var_t cpus;
	struct kvm_vcpu *vcpu;

	zalloc_cpumask_var(&cpus, GFP_ATOMIC);

	me = get_cpu();
	if (unlikely(cpus == NULL)) {
		kvm_for_each_vcpu(i, vcpu, kvm)
			kvm_make_request(KVM_REQ_TLB_FLUSH, vcpu);

		smp_call_function_many(cpu_online_mask, __noop, NULL, 1);
	} else {
		kvm_for_each_vcpu(i, vcpu, kvm) {
			kvm_make_request(KVM_REQ_TLB_FLUSH, vcpu);

			cpu = vcpu->cpu;
			if (cpu >= 0 && cpu != me &&
					(kvm_vcpu_exiting_guest_mode(vcpu)
					 != OUTSIDE_GUEST_MODE))
				cpumask_set_cpu(cpu, cpus);
		}

		if(!cpumask_empty(cpus))
			smp_call_function_many(cpus, __noop, NULL, 1);

	}
	put_cpu();

	free_cpumask_var(cpus); /* if cpus == NULL, this does nothing. */
}
#endif /* CONFIG_EMP_VM */

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
 * is_proactive_list_full - Check if the proactive list is full
 * @param bvma bvma data structure
 *
 * @retval true: Full
 * @retval false: Not full
 */
static inline bool is_proactive_list_full(struct emp_mm *bvma)
{
	size_t t = bvma->ftm.proactive_pages_len;
	return ((size_t)atomic_read(&bvma->ftm.proactive_list.page_len) >= t);
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
	size_t t = bvma->ftm.active_pages_len;
	return ((size_t)atomic_read(&bvma->ftm.active_list.page_len) >= t);
}

/**
 * is_inactive_list_full - Check if the inactive list is full
 * @param bvma bvma data structure
 *
 * @retval true: Full
 * @retval false: Not full
 */
static inline bool is_inactive_list_full(struct emp_mm *bvma)
{
	size_t t = bvma->ftm.inactive_pages_len;
	return ((size_t)read_inactive_list_page_len(bvma) >= t);
}

/**
 * get_pressure_of_inactive_list - Check number of pages belongs to the inactive list that need to be moved to writebacke list
 * @param bvma bvma data structure
 * @param hard_pressure number of pages need to be reclaimed
 * @param soft_pressure (for return) number of pages may need to be writebacked
 */
static inline void get_pressure_of_inactive_list(struct emp_mm *bvma, int *hard_pressure, int *soft_pressure)
{
	// Target length of writeback list is 1/4 of inactive list length
	size_t soft_target = bvma->ftm.inactive_keep_pages_len;
	size_t hard_target = bvma->ftm.inactive_pages_len;
	size_t wb_len = (size_t) read_inflight_writeback_page_len(bvma);
	size_t inactive_len = (size_t) read_inactive_list_page_len(bvma);
	size_t soft_len = inactive_len - wb_len + *hard_pressure;
	size_t hard_len = inactive_len + *hard_pressure;
	*soft_pressure = soft_len > soft_target ? soft_len - soft_target : 0;
	*hard_pressure = hard_len > hard_target ? hard_len - hard_target : 0;
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
		if (emp_trylock_local_page(emm, lp) == NULL) {
			if ((++num_fail) >= lb_size)
				break;
			else
				continue;
		}
		debug_assert(is_local_page_on_global(lp));
		emp_list_del(cur, global_list);
		clear_local_page_on_global(lp);
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
			if (emp_trylock_local_page(emm, lp) == NULL) {
				if ((++num_fail) >= lb_size)
					break;
				else
					continue;
			}
			debug_assert(is_local_page_on_mru(lp));
			emp_list_del(cur, mru_list);
			clear_local_page_on_mru(lp);
			temp_list_add_tail(cur, &to_lru);
			if ((++num_pop) >= lb_size)
				break;
		}
		emp_list_unlock(mru_list);
	}

	emp_list_lock(lru_list);
	temp_list_for_each_safe(cur, n, &to_lru) {
		lp = __get_local_page_from_list(cur);
		temp_list_del(cur, &to_lru);
		set_local_page_cpu_lru(lp, cpu_id);
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
		if (emp_trylock_local_page(emm, lp)) {
			debug_assert(is_local_page_on_mru(lp));
			emp_list_del(cur, mru_list);
			clear_local_page_on_mru(lp);
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
			set_local_page_cpu_mru(lp, cpu_id);
			emp_list_add_tail(cur, mru_list);
			emp_unlock_local_page(emm, lp);
		}
		emp_list_unlock(mru_list);
		return;
	}

	temp_list_for_each_safe(cur, n, &to_global) {
		lp = __get_local_page_from_list(cur);
		temp_list_del(cur, &to_global);
		set_local_page_global(lp);
		emp_list_add_tail(cur, global_list);
		emp_unlock_local_page(emm, lp);
	}
	emp_list_unlock(global_list);
}

/**
 * add_gpas_to_active_list - Add given gpas to the (pro)active list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param gpas gpas to add
 * @param n_new the number of gpas
 *
 * @return the length of new pages
 *
 * I think that this function should be separated for higher performance
 */
int add_gpas_to_active_list(enum lru_list_type lru_list_type,
				struct emp_mm *bvma, struct vcpu_var *cpu,
			 	struct emp_gpa **gpas, int n_new)
{
	/* proactive list or active list */
	struct slru *slru;
	struct emp_list *list;
	struct temp_list temp_mru;
	int new_mru_len = 0;
	struct temp_list temp_pin;
	int new_pin_len = 0;
	int i, cpu_id;

	debug_add_gpas_to_active_list(gpas, n_new, lru_list_type);

	switch (lru_list_type) {
	case PROACTIVE_LIST:
		slru = &bvma->ftm.proactive_list;
		break;
	case ACTIVE_LIST:
		slru = &bvma->ftm.active_list;
		break;
	default:
		BUG();
	}

	cpu_id = cpu->id;
	init_temp_list(&temp_mru);
	init_temp_list(&temp_pin);


	for (i = 0; i < n_new; i++) {
		struct emp_gpa *head;
		struct emp_gpa *gpa = gpas[i];

		head = emp_get_block_head(gpa);
		if (!is_unmapped_active(head))
			head->r_state = GPA_ACTIVE;
		if (unlikely(is_gpa_flags_set(head, GPA_PINNED_MASK))) {
			// pin_list is a part of proactive list
			set_gpa_flags_if_unset(head, GPA_PROACTIVE_MASK);
			set_local_page_cpu_pin(head->local_page, cpu_id);

			temp_list_add_tail(&head->local_page->lru_list, &temp_pin);
			new_pin_len += gpa_block_size(head);
		} else {
			if (lru_list_type == PROACTIVE_LIST)
				set_gpa_flags_if_unset(head, GPA_PROACTIVE_MASK);
			set_local_page_cpu_mru(head->local_page, cpu_id);

			temp_list_add_tail(&head->local_page->lru_list, &temp_mru);
			new_mru_len += gpa_block_size(head);
		}
	}

	if (new_mru_len > 0) {
		list = get_list_ptr_mru(bvma, slru, cpu_id);
		emp_list_lock(list);
		emp_list_splice_tail(&temp_mru, list);
		emp_list_unlock(list);

		if (MRU_BUF_FULL(MRU_QUEUE_DEPTH, emp_list_len(list)))
			flush_active_mru(bvma, slru, list, cpu_id, MRU_QUEUE_DEPTH);

		atomic_add(new_mru_len, &slru->page_len);
	}

	if (new_pin_len > 0) {
		list = get_list_ptr_pin(bvma, cpu_id);
		emp_list_lock(list);
		emp_list_splice_tail(&temp_pin, list);
		emp_list_unlock(list);
		atomic_add(new_pin_len, &bvma->ftm.proactive_list.page_len);
		atomic_add(new_pin_len, &bvma->ftm.cur_pin_pages);
	}

	return new_mru_len + new_pin_len;
}

bool COMPILER_DEBUG
gpa_acquire(struct emp_gpa *head)
{
	struct emp_gpa *g;
#ifdef CONFIG_EMP_BLOCK
	struct emp_gpa *pf_sb;
#endif
	struct page *p;
	int count_page, count_max, count_def;

#ifdef CONFIG_EMP_VM
	if (unlikely(is_gpa_flags_set(head, GPA_LOWMEM_BLOCK_MASK)))
		return false;
#endif

	if (!is_gpa_flags_set(head, GPA_HPT_MASK))
		return true;

	if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK)) {
		count_def = 2;
#ifdef CONFIG_EMP_BLOCK
		if (is_gpa_flags_set(head, GPA_PREFETCHED_CSF_MASK))
			pf_sb = head + (head->local_page->demand_offset
						>> gpa_subblock_order(head));
		else
			pf_sb = NULL;
#endif
	} else {
		count_def = 1;
	}

	for_each_gpas(g, head) {
		p = g->local_page->page;

		count_page = emp_page_count(p);
		count_max = count_def + g->local_page->page_map_count;

#ifdef CONFIG_EMP_BLOCK
		// prefetched subblock in CSF has been handled its I/O
		if (g == pf_sb)
			count_max -= 1;
#endif

		if (count_page > count_max) {
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
			int refcnt, mapcnt;
			if (!is_debug_page_ref_correct(g->local_page, &refcnt,
								&mapcnt, 0)) {
				struct local_page *lp = g->local_page;
				emp_debug_bulk_msg_lock();
				printk(KERN_ERR "DEBUG: [CANNOT_ACQUIRE] (%s) "
						"lp: %016lx vmr: %d gpa: %lx "
						"curr: %2d sum: %2d map: %2d "
						"mmu: %d io: %d pte: %d "
						"unmap: %d count_page: %d "
						"count_max: %d\n",
						__func__,
						(unsigned long) lp,
						emp_vmr_dbgid(emp_lp_owner(lp)),
						lp->gpa_index,
						refcnt,
						lp->debug_page_ref_sum,
						mapcnt,
						lp->debug_page_ref_in_mmu_noti,
						lp->debug_page_ref_in_io,
						lp->debug_page_ref_in_will_pte,
						lp->debug_page_ref_in_unmap,
						count_page, count_max);
				__debug_page_ref_print_all(lp);
				emp_debug_bulk_msg_unlock();
			}
#endif
			return false;
		}
	}

	return true;
}

void wait_for_prefetched_block(struct emp_mm *emm,
				struct vcpu_var *cpu, struct emp_gpa *head)
{
	struct emp_gpa *g;
#ifdef CONFIG_EMP_BLOCK
	struct emp_gpa *pf_sb_head;
#endif

	debug_assert(__is_gpa_flags_set(head, GPA_PREFETCHED_MASK));
	debug_assert(!__is_gpa_flags_same(head, GPA_nPT_MASK, 0));

#ifdef CONFIG_EMP_BLOCK
	if (is_gpa_flags_set(head, GPA_PREFETCHED_CSF_MASK))
		pf_sb_head = head + (head->local_page->demand_offset
					>> gpa_subblock_order(head));
	else
		pf_sb_head = NULL;
#endif

	for_each_gpas(g, head) {
#ifdef CONFIG_EMP_BLOCK
		if (g == pf_sb_head)
			continue;
#endif

		if (emm->sops.wait_read_async(emm, cpu, g))
			clear_gpa_flags_if_set(g, GPA_REMOTE_MASK);
	}

	clear_gpa_flags_if_set(head, GPA_PREFETCHED_MASK);
	head->local_page->demand_offset = 0;
}

static void wait_for_prefetched_blocks(struct emp_mm *emm,
		struct vcpu_var *cpu, struct emp_gpa **heads, int num_head)
{
	int i;
	struct emp_gpa *head;
	for (i = 0; i < num_head; i++) {
		head = heads[i];
		if (!is_gpa_flags_set(head, GPA_PREFETCHED_MASK))
			continue;
		wait_for_prefetched_block(emm, cpu, head);
		emp_stat_inc(emm, csf_useful);
	}
}

#define CONFIG_EMP_PROMOTE_TO_PROACTIVE
static void __promote_gpas(struct emp_mm *emm, int cpu, struct temp_list *lp_list,
				int pages_len, bool mapped)
{
	struct slru *target;
	struct emp_list *list;
	struct list_head *cur, *n;
	struct local_page *lp;
	struct emp_gpa *gpa;
#ifdef CONFIG_EMP_PROMOTE_TO_PROACTIVE
	target = &emm->ftm.proactive_list;
#else
	target = &emm->ftm.active_list;
#endif
	list = get_list_ptr_mru(emm, target, cpu);
	emp_list_lock(list);
	temp_list_for_each_safe(cur, n, lp_list) {
		lp = __get_local_page_from_list(cur);
		gpa = __get_gpa_from_local_page(emm, lp);
		temp_list_del(cur, lp_list);
		set_local_page_cpu_mru(lp, cpu);
		emp_list_add_tail(cur, list);
#ifdef CONFIG_EMP_PROMOTE_TO_PROACTIVE
		set_gpa_flags_if_unset(gpa, GPA_PROACTIVE_MASK);
#endif
		gpa->r_state = GPA_ACTIVE;
		if (!mapped) {
			set_gpa_flags_if_unset(gpa, GPA_PREFETCHED_BLK_MASK
							| GPA_PREFETCH_ONCE_MASK);
		}
		emp_unlock_local_page(emm, lp);
	}
	emp_list_unlock(list);
	atomic_add(pages_len, &target->page_len);

	if (emp_list_len(list) >= MRU_QUEUE_DEPTH)
		flush_active_mru(emm, target, list, cpu, MRU_QUEUE_DEPTH);
}

/**
 * __move_to_pin_list - move the gpas in @lp_list to the pin list
 * @param emm emm data structure
 * @param cpu working vcpu ID
 * @param lp_list list of the local pages to be pinned
 * @param from_proactive are the gpas from the proactive list?
 * @param mapped are the gpas mapped?
 *
 * pin_list is a part of the proactive list. If the gpas are not from the
 * proactive list, the length of the proactive list should be increased.
 */
static void __move_to_pin_list(struct emp_mm *emm, int cpu,
		struct temp_list *lp_list, bool from_proactive, bool mapped)
{
	struct emp_list *list;
	struct list_head *cur, *n;
	struct local_page *lp;
	struct emp_gpa *gpa;
	int pages_len = 0;
	int total_pages_len = 0;

	list = get_list_ptr_pin(emm, cpu);
	emp_list_lock(list);

	temp_list_for_each_safe(cur, n, lp_list) {
		lp = __get_local_page_from_list(cur);
		gpa = __get_gpa_from_local_page(emm, lp);
		debug_assert(is_gpa_flags_set(gpa, GPA_PINNED_MASK));
		temp_list_del(cur, lp_list);
		set_local_page_cpu_pin(lp, cpu);
		emp_list_add_tail(cur, list);

		if (!from_proactive) {
			debug_assert(!is_gpa_flags_set(gpa, GPA_PROACTIVE_MASK));
			set_gpa_flags_if_unset(gpa, GPA_PROACTIVE_MASK);
			pages_len += gpa_block_size(gpa);
		} else {
			total_pages_len += gpa_block_size(gpa);
		}

		if (!mapped) {
			gpa->r_state = GPA_ACTIVE;
			set_gpa_flags_if_unset(gpa, GPA_PREFETCHED_BLK_MASK
							| GPA_PREFETCH_ONCE_MASK
							| GPA_HPT_MASK);
		} else
			debug_assert(gpa->r_state == GPA_ACTIVE);

		emp_unlock_local_page(emm, lp);
	}

	emp_list_unlock(list);

	if (!from_proactive) {
		struct slru *target = &emm->ftm.proactive_list;
		atomic_add(pages_len, &target->page_len);
	}

	total_pages_len += pages_len;
	atomic_add(total_pages_len, &emm->ftm.cur_pin_pages);
}

/**
 * select_victims_proactive_list - select victims from proactive list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param vs victims list
 * @param vs_len the number of victims in a list
 *
 * @return the number of victims
 */
static int COMPILER_DEBUG select_victims_proactive_list(struct emp_mm *bvma,
					 struct vcpu_var *cpu,
					 struct emp_gpa **vs, int vs_len)
{
	struct slru *target_list;
	struct emp_list *list;
	struct list_head *cur, *n;
	int n_vs = 0;
	int cpu_id = cpu->id;
	int victim_pages_size = 0;
	int retry = 0;
	int n_pin = 0;
	struct temp_list to_pin;

	/* retrieve proactive list info */
	target_list = &bvma->ftm.proactive_list;
	list = get_list_ptr_lru(bvma, target_list, cpu_id);

retry_start:
	emp_list_lock(list);

	/* select vs from proactive list */
	emp_list_for_each_safe(cur, n, list) {
		struct emp_gpa *v;
		struct local_page *lp;

		lp = __get_local_page_from_list(cur);

		if ((v = emp_trylock_local_page(bvma, lp)) == NULL)
			continue;

		debug_assert(get_local_page_cpu(lp) == cpu_id
					&& is_local_page_on_lru(lp));

		emp_list_del(&lp->lru_list, list);
		clear_local_page_on_lru(lp);

		if (unlikely(is_gpa_flags_set(v, GPA_PINNED_MASK))) {
			if (n_pin == 0)
				init_temp_list(&to_pin);
			temp_list_add_tail(cur, &to_pin);
			n_pin++;
			continue;
		}

		v->r_state = GPA_TRANS_PL;
		clear_gpa_flags_if_set(v, GPA_PROACTIVE_MASK);

		/* add a v to vs */
		*(vs + n_vs) = v;
		victim_pages_size += gpa_block_size(v);

		/* if target vs are selected, exit this loop */
		if (++n_vs >= vs_len)
			break;
	}

	debug_select_victims_pl(vs, n_vs);
	emp_list_unlock(list);

	/* update the length of target list length */
	if (n_vs) {
		atomic_sub(victim_pages_size, &target_list->page_len);
	} else if (emp_list_len(list) < LRU_QUEUE_DEPTH) {
		int refill = refill_lru_buf(bvma, target_list, list, cpu_id,
							LRU_QUEUE_DEPTH);
		// one time retry with refilled lru list
		if (refill && retry == 0) {
			retry = 1;
			goto retry_start;
		}
	}

	if (unlikely(n_pin > 0))
		// pin list is a part of proactive list. Don't adjust the length.
		__move_to_pin_list(bvma, cpu_id, &to_pin, true, true);

	return n_vs;
}

/**
 * select_victims_pin_list - select victims from pin list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param vs_len the number of recommended victims
 *
 * @return the number of victims
 *
 * The selected victims are unpinned and moved to the MRU side of the
 * proactive list. They are not returned to the caller. The caller should
 * call select_victims_proactive_list() again to reclaim them.
 */
static int COMPILER_DEBUG select_victims_pin_list(struct emp_mm *bvma,
					 struct vcpu_var *cpu, int vs_len)
{
	int cpu_id = cpu->id;
	struct emp_list *pin_list, *mru_list;
	struct temp_list temp;
	struct list_head *cur, *n;
	int n_vs;
	struct local_page *lp;
	int pages_len;

	pin_list = get_list_ptr_pin(bvma, cpu_id);

	if (atomic_read(&pin_list->len) == 0)
		return 0;

	n_vs = 0;
	pages_len = 0;
	init_temp_list(&temp);
	emp_list_lock(pin_list);

	/* select vs from pin list */
	emp_list_for_each_safe(cur, n, pin_list) {
		struct emp_gpa *v;
		struct local_page *lp;

		lp = __get_local_page_from_list(cur);

		if ((v = emp_trylock_local_page(bvma, lp)) == NULL)
			continue;

		debug_assert(get_local_page_cpu(lp) == cpu_id
					&& is_local_page_on_pin(lp));

		emp_list_del(&lp->lru_list, pin_list);
		clear_local_page_on_pin(lp);
		temp_list_add_tail(cur, &temp);
		pages_len += gpa_block_size(v);

		debug_assert(is_gpa_flags_set(v, GPA_PINNED_MASK));
		debug_assert(is_gpa_flags_set(v, GPA_PROACTIVE_MASK));
		clear_gpa_flags_if_set(v, GPA_PINNED_MASK);

		/* if target vs are selected, exit this loop */
		if (++n_vs >= vs_len)
			break;
	}
	emp_list_unlock(pin_list);

	if (n_vs == 0)
		return 0;

	atomic_sub(pages_len, &bvma->ftm.cur_pin_pages);
	atomic_sub(n_vs, &bvma->ftm.num_pin_blocks);
	atomic_add(n_vs, &bvma->ftm.num_evicted_pin_blocks);

	dprintk_ratelimited("[EMP_PIN] %d blocks are unpinned due to pressure on cpu%d\n",
			n_vs, cpu_id);
	mru_list = get_list_ptr_mru(bvma, &bvma->ftm.proactive_list, cpu_id);
	emp_list_lock(mru_list);
	temp_list_for_each_safe(cur, n, &temp) {
		lp = __get_local_page_from_list(cur);
		temp_list_del(cur, &temp);
		set_local_page_cpu_mru(lp, cpu_id);
		emp_list_add_tail(cur, mru_list);
		emp_unlock_local_page(bvma, lp);
	}
	emp_list_unlock(mru_list);

	return n_vs;
}

/**
 * update_proactive_list - select victims from proactive list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param vs victims list
 * @param vs_len the number of victims in a list
 *
 * @return the number of victims
 */
static int update_proactive_list(struct emp_mm *bvma, struct vcpu_var *local_cpu,
				 struct emp_gpa **vs, int vs_len)
{
	int n_vs = 0;
	int cpu_id;
	struct vcpu_var *cpu;

	for_all_vcpus_from(cpu, cpu_id, local_cpu, bvma) {
		n_vs += select_victims_proactive_list(bvma, cpu, vs + n_vs,
						      vs_len - n_vs);
		if (n_vs >= vs_len)
			break;
	}

	if (likely(n_vs > 0))
		return n_vs;

	/* Consider pin list for victim selection */
	if (atomic_read(&bvma->ftm.cur_pin_pages) == 0)
		return n_vs;

	for_all_vcpus_from(cpu, cpu_id, local_cpu, bvma) {
		if (select_victims_pin_list(bvma, cpu, vs_len - n_vs) == 0)
			continue;

		n_vs += select_victims_proactive_list(bvma, cpu, vs + n_vs,
						      vs_len - n_vs);
		if (n_vs >= vs_len)
			break;
	}

	return n_vs;
}

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
	struct temp_list to_promote;
	int promote_pages_len = 0;
	struct temp_list to_pin;
	int pin_pages_len = 0;
	int cpu_id;
	int retry = 0;

	int n_vs = 0, vs_pages_len = 0;

	struct slru *target_list = &bvma->ftm.active_list;

	init_temp_list(&to_promote);
	init_temp_list(&to_pin);
	cpu_id = cpu->id;
	list = get_list_ptr_lru(bvma, target_list, cpu_id);

retry_start:
	emp_list_lock(list);

	/* select vs from active list */
	emp_list_for_each_safe(cur, n, list) {
		struct emp_gpa *v;
		struct local_page *lp;

		lp = __get_local_page_from_list(cur);

		if ((v = emp_trylock_local_page(bvma, lp)) == NULL)
			continue;

		debug_assert(get_local_page_cpu(lp) == cpu_id
					&& is_local_page_on_lru(lp));

		if (unlikely(is_gpa_flags_set(v, GPA_PINNED_MASK))) {
			emp_list_del(&lp->lru_list, list);
			clear_local_page_on_lru(lp);
			temp_list_add_tail(&lp->lru_list, &to_pin);
			pin_pages_len += gpa_block_size(v);
			continue;
		}

		// check GPA_PROMOTE_MASK
		if (clear_gpa_flags_if_set(v, GPA_PROMOTE_MASK)) {
			emp_list_del(&lp->lru_list, list);
			clear_local_page_on_lru(lp);
			temp_list_add_tail(&lp->lru_list, &to_promote);
			promote_pages_len += gpa_block_size(v);
			continue;
		}

		if (!is_unmapped_active(v) && gpa_acquire(v)) {
			emp_list_del(&lp->lru_list, list);
			clear_local_page_on_lru(lp);
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

	// migrate active gpas to pin list
	if (unlikely(pin_pages_len)) {
		atomic_sub(pin_pages_len, &target_list->page_len);
		__move_to_pin_list(bvma, cpu_id, &to_pin, false, true);
	}

	// migrate active gpas to mru_buf
	if (promote_pages_len) {
		atomic_sub(promote_pages_len, &target_list->page_len);
		__promote_gpas(bvma, cpu_id, &to_promote, promote_pages_len, true);
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

	for_all_vcpus_from(cpu, cpu_id, local_cpu, emm) {
		n_vs += select_victims_active_list(emm, cpu,
				vs + n_vs,
				vs_max - n_vs);
		if (n_vs >= vs_max)
			break;
	}

	if (unlikely(n_vs == 0))
		return 0;

	/* release work requests of prefetch sub-blocks */
	wait_for_prefetched_blocks(emm, local_cpu, vs, n_vs);

	/* reclaiming gpas to move them to inactive list */
	reclaim_gpa_many(emm, vs, n_vs);
	return n_vs;
}

/**
 * check_eager_wbr - Check eager writeback block and handle the writeback
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param head head of the block
 */
void check_eager_wbr(struct emp_mm *bvma, struct vcpu_var *cpu,
		     struct emp_gpa *head)
{
	struct work_request *w;
	struct eager_wbr *e;

	if (!clear_gpa_flags_if_set(head, GPA_EAGER_WBR_MASK))
		return;

	e = (struct eager_wbr *) head->local_page->w;
	debug_check_eager_wbr(head, e->g);

	w = e->w;
	e->w = NULL;
	head->local_page->w = w;
	debug_progress(w, head);
	bvma->sops.clear_writeback_block(bvma, head, w, cpu,
					 false, false);
	head->local_page->w = NULL;
	free_eager_wbr(bvma, e);

	debug_check_eager_wbr2(bvma, head);
	/* Clear GPA_DIRTY_MASK to prevent duplicated writebacks. */
	clear_gpa_flags_if_set(head, GPA_DIRTY_MASK);
}

/**
 * clear_block_w - wait for what the block holds in @w and leave it NULL
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param head head of the block, locked
 *
 * A local page's @w means one thing at a time, and the block says which: an
 * eager_wbr while GPA_EAGER_WBR_MASK is set, the writeback work request
 * while the block is GPA_WB, or the fetch work requests of a block whose
 * prefetch is still in flight. Whoever is about to store into @w calls this
 * first, so nothing in flight is overwritten and no reader finds a pointer
 * of another kind.
 */
void clear_block_w(struct emp_mm *bvma, struct vcpu_var *cpu,
		   struct emp_gpa *head)
{
	if (is_gpa_flags_set(head, GPA_EAGER_WBR_MASK))
		check_eager_wbr(bvma, cpu, head);

	if (WB_BLOCK(head)) {
		if (head->local_page->w)
			bvma->sops.clear_writeback_block(bvma, head,
					head->local_page->w, cpu, true, false);
	} else if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK))
		wait_for_prefetched_block(bvma, cpu, head);

	debug_assert(head->local_page->w == NULL);
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
	int pc;
	bool is_free = true;

	pc = is_gpa_flags_set(head, GPA_EAGER_WBR_MASK)? 2: 1;

	for_each_gpas(g, head) {
		p = g->local_page->page;
		if (emp_page_count(p) != pc) {
			is_free = false;
			break;
		}
	}

	return is_free;
}


/**
 * select_victims_inactive_list - select victims from inactive list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param vs victims list
 * @param vs_len the number of victims in a list
 * @param ctime current time
 * @param need_tlb_flush
 * @param pressure the remaining pressure in terms of pages
 *
 * @return the number of victims
 */
static int select_victims_inactive_list(struct emp_mm *bvma,
					struct vcpu_var *cpu,
					struct emp_gpa **vs, int vs_len,
					u64 ctime, bool *need_tlb_flush,
					int *pressure)
{
	struct emp_list *list;
	struct list_head *cur, *n;
	struct slru *target_list;
	struct temp_list to_promote;
	int promote_pages_len = 0;
	struct temp_list to_pin;
	int pin_pages_len = 0;
	int n_vs = 0, cpu_id;

	init_temp_list(&to_promote);
	init_temp_list(&to_pin);
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

		if ((v = emp_trylock_local_page(bvma, lp)) == NULL)
			continue;

		debug_assert(get_local_page_cpu(lp) == cpu_id
					&& is_local_page_on_mru(lp));

		if (unlikely(is_gpa_flags_set(v, GPA_PINNED_MASK))) {
			emp_list_del(&lp->lru_list, list);
			clear_local_page_on_mru(lp);
			temp_list_add_tail(&lp->lru_list, &to_pin);
			pin_pages_len += gpa_block_size(v);
			continue;
		}

		// check GPA_PROMOTE_MASK
		if (clear_gpa_flags_if_set(v, GPA_PROMOTE_MASK)) {
			emp_list_del(&lp->lru_list, list);
			clear_local_page_on_mru(lp);
			temp_list_add_tail(&lp->lru_list, &to_promote);
			promote_pages_len += gpa_block_size(v);
			continue;
		}

		reinsert_gpa = !check_block_free(bvma, v);

		// inactive_list.len will be decreased when GPA_WB->GPA_INIT
		// by calling sub_inactive_list_page_len
		if (reinsert_gpa == false) {
			emp_list_del(&lp->lru_list, list);
			clear_local_page_on_mru(lp);
			debug_check_notlocked(v);

			v->r_state = GPA_TRANS_IL;
			vs[n_vs++] = v;
			*pressure -= gpa_block_size(v);
			if (n_vs >= vs_len)
				break;
			continue;
		}

		emp_unlock_block(v);
	}

	emp_list_unlock(list);

	if (unlikely(pin_pages_len > 0)) {
#ifdef CONFIG_EMP_DEBUG
		struct local_page *lp;
		temp_list_for_each(cur, &to_pin) {
			lp = __get_local_page_from_list(cur);
			sub_inactive_list_page_len(bvma, lp->gpa);
		}
#else
		__sub_inactive_list_page_len(pin_pages_len, bvma);
#endif
		__move_to_pin_list(bvma, cpu_id, &to_pin, false, false);
	}

	if (promote_pages_len) {
#ifdef CONFIG_EMP_DEBUG
		struct local_page *lp;
		temp_list_for_each(cur, &to_promote) {
			lp = __get_local_page_from_list(cur);
			sub_inactive_list_page_len(bvma, lp->gpa);
		}
#else
		__sub_inactive_list_page_len(promote_pages_len, bvma);
#endif
		__promote_gpas(bvma, cpu_id, &to_promote, promote_pages_len, false);
	}

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
#ifdef CONFIG_EMP_DEBUG
	int head_mrid;
#endif

	dma_size = min((unsigned int)gpa_block_size(head),
			(unsigned int)emm->mrs.memregs_wdma_size);
	dma_size = min((u8)dma_size, (u8)(bvma_subblock_size(emm)));

	head_wr = NULL;
	tail_wr = NULL;
	eh_wr = NULL;

	debug_evict_block(emm, head);

	clear_block_w(emm, cpu, head);

	if (!alloc_remote_page(emm, head))
		goto error;

	ew_len = 0;
#ifdef CONFIG_EMP_DEBUG
	head_mrid = get_gpa_remote_page_mrid(head);
#endif

	for_each_gpas(victim, head) {
#ifdef CONFIG_EMP_DEBUG
		if (emm->config.remote_policy_subblock == REMOTE_POLICY_SUBBLOCK_SAME_MR
				&& get_gpa_remote_page_mrid(victim) != head_mrid)
			printk(KERN_ERR "%s different mrid within a block victim: %d head: %d\n",
					__func__, get_gpa_remote_page_mrid(victim), head_mrid);
#endif

		debug_page_ref_io_beg(victim->local_page);
		emp_get_subblock(victim);
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

		emp_vcpu_stat_add(cpu, wb_count, dma_size);
	}

	if (head_wr && head_wr->chained_ops) {
		struct dma_ops *ops = head_wr->dma_ops;
		ops->post_work_req(head_wr, tail_wr, head_wr, ew_len);
	}

	if (!eager_evict) {
		head->r_state = GPA_WB;
		emm->sops.push_writeback_request(emm, head_wr, cpu);
	}

	emp_stat_inc(emm, post_write_count);
	return head_wr;

error:
	if (head_wr == NULL) {
		if (victim != NULL) {
			emp_unlock_subblock(head);
			debug_page_ref_io_end(head->local_page);
			emp_put_subblock(head);
		}
	} else {
		bool chained_ops = head_wr->chained_ops;
		struct emp_gpa *gpa;
		while (list_empty(chained_ops ? &head_wr->remote_pages
						: &head_wr->subsibling)) {
			if (chained_ops)
				w = list_last_entry(&head_wr->remote_pages,
							struct work_request,
							remote_pages);
			else
				w = list_last_entry(&head_wr->subsibling,
							struct work_request,
							subsibling);
			list_del_init(&w->remote_pages);
			/* for chained_ops, the writes are not actually posted */
			if (chained_ops)
				destroy_write(emm, w);
			else
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
 * check_need_writeback - Check if the block needs writeback
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param head head of the block
 *
 * @retval true: Need writeback
 * @retval false: No need to writeback
 */
static bool check_need_writeback(struct emp_mm *bvma, struct vcpu_var *cpu,
				 struct emp_gpa *head)
{
	bool need_writeback;

	if (bvma->config.writeback_optimization_disable)
		return true;

	/* Even if a page is clean, the page should be evicted to
	 * remote memory when the remote reuse option is off
	 */
	if (!bvma->config.remote_reuse)
		return true;

	if (!is_block_remote_page_valid(head))
		return true;

	need_writeback = clear_gpa_flags_if_set(head, GPA_DIRTY_MASK);
	if (need_writeback)
		emp_vcpu_stat_inc(cpu, dbit_count);
	else
		emp_vcpu_stat_inc(cpu, cbit_count);

	return need_writeback;
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
	int ret;
#ifdef CONFIG_EMP_OPT
	struct emp_gpa *p;
	bool need_writeback;

	need_writeback = check_need_writeback(emm, cpu, head);
	// dbit_count is increased because a block granularity
	// 'd' bit is set

	if (emm->config.writeback_optimization_disable ||
			need_writeback) {
		// this case is for dirty bit set and detection of
		// update for the entire gpas by using hash, and it also
		// covers writeback_optimization_disabled == true
		ret = PTR_ERR_RET(evict_block(emm, cpu, head, false));
	} else if (!need_writeback) {
		// no need to writeback any gpas because dbit is clean
		// so we know that the entire gpas are not updated

		// change the state of the entrire gpas into GPA_INIT,
		// release related resources, and set GPA_REMOTE_MASK flag
		for_each_gpas_reverse(p, head) {
			emm->vops.set_gpa_remote(emm, cpu, p);
		}
		sub_inactive_list_page_len(emm, head);
		ret = 0;
	} else {
		// it covers dirty bit is set, however some of gpas might 
		// not to be write back because hash value is identical
		// with concern of hash collision, a gpa, which has
		// an identical hash value to the page content on xpoint,
		// must be compared completely

		ret = PTR_ERR_RET(evict_block(emm, cpu, head, false));
		// if the entire pages are sure not to be written back,
		// it frees these gpas in a block and discharges the
		// length of the inactive list.
		if (ret == 0) {
			for_each_gpas_reverse(p, head) {
				emm->vops.set_gpa_remote(emm, cpu, p);
			}
			sub_inactive_list_page_len(emm, head);
		}
	}
#else /* !CONFIG_EMP_OPT */
	ret = PTR_ERR_RET(evict_block(emm, cpu, head, false));
#endif /* !CONFIG_EMP_OPT */

	return ret;
}

#ifdef CONFIG_EMP_ELASTIC_BLOCK
/**
 * reduce_and_writeback - Reduce a block size and writeback
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 *
 * @return the number of writeback pages
 */
static int reduce_and_writeback(struct emp_mm *bvma, struct vcpu_var *cpu,
				struct emp_gpa *head)
{
	int i, n_wb_pages = 0;
	int hs_len = 1 << gpa_desc_order(head);
	int writeback_done;
	struct emp_gpa *hs[hs_len];

	if (els_tryreduce_complete(bvma, head, hs) == false) {
		if (els_reduce(bvma, head, hs)) {
			hs_len = 2;
		} else {
			hs[0] = head;
			hs_len = 1;
		}
	}

	debug_reduce_inactive_list_page_len(bvma, hs, hs_len);

	for (i = 0; i < hs_len; i++) {
		writeback_done = emp_writeback_block(bvma, hs[i], cpu);
		if (unlikely(writeback_done < 0))
			goto error;
		if (writeback_done) {
			n_wb_pages += gpa_block_size(hs[i]);
			emp_els_stat_inc(bvma, hs[i], writeback);
		}
		emp_unlock_block(hs[i]);
	}

#ifdef CONFIG_EMP_BLOCKDEV
	if (bvma->mrs.blockdev_used)
		io_schedule();
#endif

	return n_wb_pages;

error:
	for ( ; i < hs_len; i++)
		emp_unlock_block(hs[i]);
#ifdef CONFIG_EMP_BLOCKDEV
	if (bvma->mrs.blockdev_used)
		io_schedule();
#endif
	return writeback_done;
}

/**
 * els_need_reduce - Check the block needs to be reduced
 * @param bvma bvma data structure
 * @param head head of the block
 * @param ref_count reference count of the block
 *
 * @retval true: Need to reduce
 * @retval false: No need to reduce
 */
static inline bool 
els_need_reduce(struct emp_mm *emm, struct emp_gpa *head, int ref_count)
{
	if (emm->config.els_disabled)
		return false;

	if (gpa_block_order(head) <= gpa_subblock_order(head))
		return false;

	if (head->local_page && head->local_page->w)
		return false;

	if (ref_count > (num_subblock_in_block(head) >> ELS_REDUCE_THRESHOLD_ORDER))
		return false;

	return true;
}

/**
 * els_reduce_and_writeback_block - Reduce the elastic block size and writeback
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param head head of the block
 *
 * @retval 1: reduce
 * @retval 0: no reduce
 * @retval -1: error
 */
static int els_reduce_and_writeback_block(struct emp_mm *bvma,
					   struct vcpu_var *cpu,
					   struct emp_gpa *head)
{
	struct emp_gpa *v;
	unsigned int ref_count = 0;
	unsigned int noref_count = 0;
	int ret = 0;

	for_each_gpas(v, head) {
		// referenced flags will be cleared by set_gpa_remotified 
		if (is_gpa_flags_set(v, GPA_REFERENCED_MASK))
			ref_count++;
		else
			noref_count++;
	}

	emp_els_stat_add(bvma, head, ref_count, ref_count);
	emp_els_stat_add(bvma, head, noref_count, noref_count);

	if (els_need_reduce(bvma, head, ref_count)) {
		ret = reduce_and_writeback(bvma, cpu, head);
		if (ret >= 0)
			ret = 1; /* if there is no error, @head was reduced */
	}

	return ret;
}
#endif /* CONFIG_EMP_ELASTIC_BLOCK */

/**
 * update_inactive_list - select victims from inactive list and writeback
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param pressure number of pages that should be writebacked
 *
 * @return the number of victims
 *
 * Select victims in inactive list and writeback the victims to donor
 */
static int update_inactive_list(struct emp_mm *bvma, struct vcpu_var *local_cpu,
				int hard_pressure, int soft_pressure)
{
	struct emp_gpa *victims[NUM_VICTIM_CLUSTER], **vs;
	struct vcpu_var *cpu;
	int cpu_id;
	struct emp_list *list;
	int n_victims = 0, n_victim_pages = 0, n_vs, pressure;
	int i;
	struct slru *inactive_list = &bvma->ftm.inactive_list;
#ifdef CONFIG_EMP_VM
	bool need_tlb_flush;
	u64 ctime, t_tlb_flush;

	ctime = get_ts_in_ns();
	need_tlb_flush = false;
	t_tlb_flush = local_cpu->t_tlb_flush;
#endif /* CONFIG_EMP_VM */

	for_all_vcpus_from(cpu, cpu_id, local_cpu, bvma) {
		int c = cpu->id;
		list = get_list_ptr_inactive(bvma, inactive_list, c);
		if (emp_list_len(list) == 0)
			continue;
		vs = victims + n_victims;
		n_vs = NUM_VICTIM_CLUSTER - n_victims;
		pressure = soft_pressure;
#ifdef CONFIG_EMP_VM
		n_victims += select_victims_inactive_list(bvma, cpu, vs, n_vs,
						ctime, &need_tlb_flush, &pressure);
#else
		n_victims += select_victims_inactive_list(bvma, cpu, vs, n_vs,
						0, NULL, &pressure);
#endif
		hard_pressure -= soft_pressure - pressure;
		// We tried to reduce soft_pressure, but only hard_pressure is checked.
		// If hard_pressure is initially 0, we do not check other cpus and only scan the local one.
		if (n_victims >= NUM_VICTIM_CLUSTER || hard_pressure <= 0)
			break;
		soft_pressure = pressure;
	}

#ifdef CONFIG_EMP_VM
	if (need_tlb_flush && is_emm_with_kvm(bvma)) {
		tlb_flush_all(bvma->ekvm.kvm);
		emp_stat_inc(bvma, remote_tlb_flush_force);
		local_cpu->t_tlb_flush = ctime;
	}
#endif /* CONFIG_EMP_VM */

	for (i = 0; i < n_victims; i++) {
		// head of victims[i] is locked at select_victims_inactive_list
		int writeback_done;
		struct emp_gpa *head = victims[i];

		debug_update_inactive_list(head,
				emp_get_block_head(victims[i]));

		check_eager_wbr(bvma, local_cpu, head);
		debug_update_inactive_list2(bvma, head);

		n_victim_pages += gpa_block_size(head);
#ifdef CONFIG_EMP_ELASTIC_BLOCK
		writeback_done = els_reduce_and_writeback_block(bvma,
							local_cpu, head);
		if (unlikely(writeback_done < 0)) {
			n_victim_pages = writeback_done;
			goto error;
		}
		if (writeback_done)
			continue;
#endif /* CONFIG_EMP_ELASTIC_BLOCK */

		writeback_done = emp_writeback_block(bvma, head, local_cpu);
		if (unlikely(writeback_done < 0)) {
			n_victim_pages = writeback_done; /* error code */
			goto error;
		}

#ifdef CONFIG_EMP_ELASTIC_BLOCK
		if (writeback_done)
			emp_els_stat_inc(bvma, head, writeback);
#endif /* CONFIG_EMP_ELASTIC_BLOCK */

		debug_progress(head, writeback_done);
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
 * do_eager_writeback - Do writeback unmapped dirty block eagerly
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param d_heads dirty block heads
 * @param n_d_heads the number of dirty blocks
 *
 * Eager writeback makes a headroom for the local memory. \n
 * It allocates eager writeback request & writeback unmapped dirty block
 */
static int do_eager_writeback(struct emp_mm *bvma, struct vcpu_var *cpu,
			       struct emp_gpa *d_heads[], int n_d_heads)
{
	int i;
	struct emp_gpa *head;
	bool not_free;
	struct work_request *w;
	struct eager_wbr *e;

	for (i = 0; i < n_d_heads; i++) {
		head = d_heads[i];

		not_free = !check_block_free(bvma, head);
		if (not_free) {
			debug_do_eager_writeback(head);
			continue;
		}

		e = alloc_eager_wbr(cpu);
		debug_check_null_pointer(e);
		w = evict_block(bvma, cpu, head, true);
		if (!w) {
			free_eager_wbr(bvma, e);
			continue;
		} else if (unlikely(IS_ERR(w)))
			goto error;

		e->g = head;
		e->w = w;
		debug_check_notnull_pointer(head->local_page->w);
		head->local_page->w = (struct work_request *) e;
		set_gpa_flags_if_unset(head, GPA_EAGER_WBR_MASK);
		emp_els_stat_inc(bvma, head, writeback);
	}

	return 0;

error:
	free_eager_wbr(bvma, e);
	return (int) PTR_ERR(w);
}

struct eager_wbr *
alloc_eager_wbr(struct vcpu_var *cpu)
{
	struct eager_wbr *w;
	w = emp_kmem_cache_alloc(cpu->eager_wbr_cache, GFP_ATOMIC);
	w->cpu = cpu->id;
	return w;
}

void free_eager_wbr(struct emp_mm *emm, struct eager_wbr *w)
{
	struct vcpu_var *v;
	v = emp_get_vcpu_from_id(emm, w->cpu);
	emp_kmem_cache_free(v->eager_wbr_cache, w);
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
	int ret;
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
		set_local_page_cpu_mru(head->local_page, cpu_id);
		emp_list_add_tail(&head->local_page->lru_list, list);
		debug_add_inactive_list_page_len(bvma, head);
		new_pages_len += gpa_block_size(head);
	}
	emp_list_unlock(list);

	if (new_pages_len)
		__add_inactive_list_page_len(new_pages_len, bvma);

	ret = do_eager_writeback(bvma, cpu, gpas, n_new);
	if (unlikely(ret < 0))
		return ret;

	return new_pages_len;
}

/**
 * _update_lru_lists - Update LRU lists
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 * @param pressure pressure
 *
 * Check the number of pages in LRU lists and move the pages among the lists
 * if the lists are full
 */
static int _update_lru_lists(struct emp_mm *bvma, struct vcpu_var *cpu,
				int pressure)
{
	int i, ret = 0, soft_pressure;
	int n_victim = 0, n_unmap_pages, n_released_pages; // TODO: merge these variables
	struct emp_gpa *victims[NUM_VICTIM_CLUSTER];

	// for proactive queue
	if (is_proactive_list_full(bvma)) {
#ifndef CONFIG_EMP_SEQUENTIAL
		// increase m after pressure of proactive queue occurred once
		if (unlikely(bvma->ftm.divider == 1))
			bvma->ftm.divider = 10;
#endif
		n_victim = update_proactive_list(bvma, cpu, victims,
				NUM_VICTIM_CLUSTER);
		if (n_victim > 0) {
			add_gpas_to_active_list(ACTIVE_LIST, bvma, cpu, victims, n_victim);
			for (i = 0; i < n_victim; i++)
				emp_unlock_block(victims[i]);
		}
	}

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
	get_pressure_of_inactive_list(bvma, &pressure, &soft_pressure);
	if (soft_pressure > 0) {
		ret = update_inactive_list(bvma, cpu, pressure, soft_pressure);
		if (unlikely(ret < 0))
			return ret;
		n_released_pages = ret;
	}

	return n_released_pages;
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
 * (from inactive list to proactive list)
 */
/* functions to update lru lists of gpas */
int update_lru_lists_reref(struct emp_mm *b, struct vcpu_var *cpu,
			   struct emp_gpa **new_gs, int n_new_gs,
			   const int pressure)
{
	if (n_new_gs)
		add_gpas_to_active_list(PROACTIVE_LIST, b, cpu, new_gs, n_new_gs);
	return _update_lru_lists(b, cpu, 0);
}

/**
 * update_lru_lists - Update LRU lists when the blocks are fetched to local memory
 * @param b bvma data structure
 * @param v working vcpu ID
 * @param new_gs new blocks
 * @param n_new_gs the number of new blocks
 * @param pressure block size
 *
 * It selects and evicts a victim by updating LRU lists.
 * When the block is inserted to the lists,
 * it uses epsilon and divider to fully utilize proactive memory region.
 * This makes the block to insert new gpa into proactive list
 * at 10% ratio by PROACTIVE_FILTER()
 */
int update_lru_lists(struct emp_mm *b, struct vcpu_var *cpu,
		     struct emp_gpa **new_gs, int n_new_gs, int pressure)
{
	if (n_new_gs == 0)
		return _update_lru_lists(b, cpu, 0);

#ifdef CONFIG_EMP_SEQUENTIAL
	if (pressure < 0)
		pressure = -pressure;
	add_gpas_to_active_list(PROACTIVE_LIST, b, cpu, new_gs, n_new_gs);
#else
	if (pressure >= 0) {
		/* Using epsilon and divider to fully utilize proactive memory region.
		 * 
		 * Since epsilon and divider are initialized to 0 and 1,
		 * this code block will try to insert all new gpas into
		 * proactive list.
		 *
		 * After that, divider will be set to 10.
		 * This makes the code block to insert new gpa into proactive list
		 * at 10% ratio.
		 * */
		if (PROACTIVE_FILTER(b))
			add_gpas_to_active_list(PROACTIVE_LIST, b, cpu, new_gs,
					     n_new_gs);
		else
			add_gpas_to_active_list(ACTIVE_LIST, b, cpu, new_gs,
					     n_new_gs);
	} else { // pressure <= 0 => called from blk_pf_ctx_flush()
		// This is the prefetched blocks. Add them to proactive list.
		pressure = -pressure;
		add_gpas_to_active_list(PROACTIVE_LIST, b, cpu, new_gs, n_new_gs);
	}
#endif
	return _update_lru_lists(b, cpu, 0);
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
	int r, ret, soft_pressure;

	if (n_new_gs) {
		r = add_gpas_to_inactive(b, cpu, new_gs, n_new_gs);
		if (unlikely(r < 0))
			return r;
	}

	ret = 0;
	get_pressure_of_inactive_list(b, &pressure, &soft_pressure);
	if (soft_pressure > 0) {
		r = update_inactive_list(b, cpu, pressure, soft_pressure);
		if (unlikely(r < 0))
			return r;
		ret += r;
	}
	return ret;
}

/**
 * reclaim_gpa_many - Reclaim mutliple gpas
 * @param bvma bvma data structure
 * @param gpas gpas to reclaim
 * @param n_gpas the number of gpas
*/
void reclaim_gpa_many(struct emp_mm *bvma, struct emp_gpa *gpas[], int n_gpas)
{
	int i, end;
	bool tlb_flush_force = false;

	if (unlikely(n_gpas <= 0))
		return;

	/* TODO: if some gpas are contiguous entries, it may improve
	 * the latency that grouping the entries and calling
	 * unmap_sptes() once for the group.
	 * However, please double check the probabilty of grouping
	 * and the reduced latency. The reduced latency may be negative.
	 */
	end = n_gpas - 1; // always larger than or equal to 0
	for (i = 0; i < end; i++)
		bvma->vops.unmap_gpas(bvma, gpas[i], &tlb_flush_force);

	if (!bvma->config.async_invlept)
		tlb_flush_force = true;

	bvma->vops.unmap_gpas(bvma, gpas[end], &tlb_flush_force);

	emp_stat_add(bvma, recl_count, n_gpas);
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
int reclaim_emp_pages(struct emp_mm *emm, struct vcpu_var *cpu, int pressure)
{
	int ret, released = 0;
	int reclaimed = emp_wait_for_writeback(emm, cpu, pressure);

	if (reclaimed >= pressure)
		return reclaimed;

	pressure -= reclaimed;
	ret = update_inactive_list(emm, cpu, pressure, pressure);
	if (unlikely(ret < 0))
		return ret;
	released += ret;

	if (released < pressure) {
		ret = _update_lru_lists(emm, cpu, pressure - released);
		if (unlikely(ret < 0))
			return ret;
		released += ret;
	}

	// We did emp_wait_for_writeback(), so there are only @released pages on writeback list.
	reclaimed += emp_wait_for_writeback(emm, cpu, released);
	return reclaimed;
}

int adjust_local_cache_size(struct emp_mm *bvma, ssize_t diff_size, ssize_t new_size) {
	ssize_t curr_pages = atomic_read(&bvma->ftm.local_cache_pages);
	ssize_t curr_size = __PAGES_TO_SIZE_VM(curr_pages, bvma);
	int sign;
	long diff_pages;

	if (diff_size != 0)
		new_size = curr_size + diff_size;
	else if (new_size != 0 && new_size != curr_size)
		diff_size = new_size - curr_size;
	else
		return 0;

	if (new_size < __PAGES_TO_SIZE_VM(1, bvma)) {
		printk(KERN_ERR "ERROR: initial_local_cache_size should be larger than 0x%lx (subblock size)\n",
					__PAGES_TO_SIZE_VM(1, bvma));
		return -EINVAL;
	}

	if (new_size < __PAGES_TO_SIZE_VM(bvma->config.minimum_pages, bvma)) {
		printk(KERN_WARNING "%s: the requested size(0x%lx) is less than minimum. "
						"the local cache size(%lx) will be changed to the minimum size(0x%lx).\n",
						__func__, new_size, curr_size,
						__PAGES_TO_SIZE_VM(bvma->config.minimum_pages, bvma));
		if (curr_pages == bvma->config.minimum_pages)
			return 0;
		new_size = __PAGES_TO_SIZE_VM(bvma->config.minimum_pages, bvma);
		diff_size = new_size - curr_size;
	}

	sign = diff_size > 0 ? 1 : -1;
	diff_size = diff_size * sign; /* take an absolute value */
	diff_pages = __SIZE_TO_PAGES_VM(diff_size, bvma);

	dprintk(KERN_ERR "change in local cache size(before): emp_id: %d local_pages: %x proactive_pages_len: %lx active_page_len: %lx inactive_pages_len: %lx\n",
				bvma->id,
				atomic_read(&bvma->ftm.local_cache_pages),
				bvma->ftm.proactive_pages_len,
				bvma->ftm.active_pages_len,
				bvma->ftm.inactive_pages_len);
	atomic_add(sign * diff_pages, &bvma->ftm.local_cache_pages);
#ifndef CONFIG_EMP_DEBUG
	printk(KERN_INFO "change in local cache size: emp_id: %d value: 0x%lx -> 0x%lx\n",
				bvma->id, curr_size, new_size);
#endif
	reclaim_set(bvma);
	dprintk(KERN_ERR "change in local cache size(after): emp_id: %d local_pages: %x proactive_pages_len: %lx active_page_len: %lx inactive_pages_len: %lx\n",
				bvma->id,
				atomic_read(&bvma->ftm.local_cache_pages),
				bvma->ftm.proactive_pages_len,
				bvma->ftm.active_pages_len,
				bvma->ftm.inactive_pages_len);

	if (sign > 0) {
		wake_up_interruptible(&bvma->ftm.free_pages_wq);
	} else {
		int reclaimed = 1;
		struct vcpu_var *cpu = emp_this_cpu_ptr(bvma->pcpus);
		atomic_add(diff_pages, &bvma->ftm.free_pages_reclaim);

		while (reclaimed > 0) {
			reclaimed = reclaim_emp_pages(bvma, cpu, diff_pages);
			if (reclaimed <= 0)
				break;
			diff_pages -= reclaimed;
		}
	}

	return 0;
}

/* remove gpa from proactive list */
static void __remove_from_proactive(struct emp_mm *emm, struct emp_gpa *gpa) {
	struct slru *proactive = &emm->ftm.proactive_list;
	struct emp_list *list;
	struct local_page *lp = gpa->local_page;

	debug_assert(lp);
	/* We checked GPA_PROACTIVE_MASK */
	debug_assert(is_local_page_on_list(lp) || is_local_page_on_pin(lp));

	debug_assert(!list_empty(&lp->lru_list));
	debug_assert(lp->lru_list.next != LIST_POISON1 && lp->lru_list.prev != LIST_POISON2);

	if (is_local_page_on_pin(lp)) {
		list = get_list_ptr_pin(emm, lp->cpu);
		atomic_sub(gpa_block_size(gpa), &emm->ftm.cur_pin_pages);
	} else if (is_local_page_on_mru(lp)) {
		list = get_list_ptr_mru(emm, proactive, lp->cpu);
	} else if (is_local_page_on_lru(lp)) {
		list = get_list_ptr_lru(emm, proactive, lp->cpu);
	} else {
		debug_assert(is_local_page_on_global(lp));
		list = &proactive->list;
	}

	emp_list_lock(list);
	emp_list_del(&lp->lru_list, list);
	emp_list_unlock(list);
	atomic_sub(gpa_block_size(gpa), &proactive->page_len);
	if (is_local_page_on_pin(lp))
		clear_local_page_on_pin(lp);
	else
		clear_local_page_list_flags(lp);
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
	if (head->r_state == GPA_ACTIVE) {
		if (clear_gpa_flags_if_set(head, GPA_PROACTIVE_MASK))
			__remove_from_proactive(emm, head);
		else
			__remove_from_active_list(emm, head);
	} else if (head->r_state == GPA_INACTIVE)
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
	f->inactive_pages_len = atomic_read(&f->local_cache_pages) >> 3;
	f->active_pages_len = atomic_read(&f->local_cache_pages) >> 1;

	f->proactive_pages_len = (atomic_read(&f->local_cache_pages) -
		(f->inactive_pages_len + f->active_pages_len));
#ifndef CONFIG_EMP_SEQUENTIAL
	emm->ftm.divider = 1;
#endif
}

/**
 * reclaim_init - Initialize LRU lists
 * @param bvma bvma data structure
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * EMP maintains 3 LRU lists to manage local memory efficiently
 * + Proactive list: re-referenced pages or first touch pages or pinned pages (in pin_list)
 * + Active list: active pages
 * + Inactive list: the page is located in local memory but the mapping info is disabled (unmapped)
*/
int reclaim_init(struct emp_mm *bvma)
{
	int cpu;
#ifdef CONFIG_EMP_VM
	int i, vcpu_len = EMP_KVM_VCPU_LEN(bvma);
	int buf_size = sizeof(struct emp_list) * vcpu_len;
#endif
	struct slru *proactive, *active, *inactive;
	struct emp_list *mru, *lru, *pin;

	proactive = &bvma->ftm.proactive_list;
	active = &bvma->ftm.active_list;
	inactive = &bvma->ftm.inactive_list;

	/* initialize global data */
#ifndef CONFIG_EMP_SEQUENTIAL
	atomic_set(&bvma->ftm.epsilon, 0);
#endif
	reclaim_set(bvma);
	init_emp_list(&proactive->list);
	init_emp_list(&active->list);
	init_emp_list(&inactive->list);
#ifdef CONFIG_EMP_VM
#ifdef CONFIG_EMP_DEBUG
	proactive->abuf_len = vcpu_len;
	active->abuf_len = vcpu_len;
	inactive->abuf_len = vcpu_len;
#endif /* CONFIG_EMP_DEBUG */

#ifdef CONFIG_EMP_USER
	if (vcpu_len == 0) {
		proactive->mru_bufs = NULL;
		proactive->lru_bufs = NULL;
		active->mru_bufs = NULL;
		active->lru_bufs = NULL;
		inactive->mru_bufs = NULL;
		inactive->lru_bufs = NULL;
		goto skip_alloc_bufs;
	}
#endif /* CONFIG_EMP_USER */

	/* initialize per-vcpu proactive and active_list */
	/* note: mru_bufs[0 ~ (#vcpu)] is initialized.
	 * mru_bufs[0] will not be used because we do not use cpu_id 0 
	 * lru_bufs[0] is the same */
	proactive->mru_bufs = emp_kmalloc(buf_size, GFP_KERNEL);
	proactive->lru_bufs = emp_kmalloc(buf_size, GFP_KERNEL);
	if (!proactive->mru_bufs || !proactive->lru_bufs)
		goto reclaim_init_fail;
	active->mru_bufs = emp_kmalloc(sizeof(struct emp_list) * vcpu_len, GFP_KERNEL);
	active->lru_bufs = emp_kmalloc(sizeof(struct emp_list) * vcpu_len, GFP_KERNEL);
	if (!active->mru_bufs || !active->lru_bufs)
		goto reclaim_init_fail;

	for (i = 0; i < vcpu_len; i++) {
		init_emp_list(&proactive->mru_bufs[i]);
		init_emp_list(&proactive->lru_bufs[i]);
		init_emp_list(&active->mru_bufs[i]);
		init_emp_list(&active->lru_bufs[i]);
	}

	/* initialize per-vcpu inactive_list
	 * mru_bufs holds inactive pages; lru_bufs holds writeback pages */
	inactive->mru_bufs = emp_kmalloc(sizeof(struct emp_list) * vcpu_len, GFP_KERNEL);
	inactive->lru_bufs = emp_kmalloc(sizeof(struct emp_list) * vcpu_len, GFP_KERNEL);
	if (!inactive->mru_bufs || !inactive->lru_bufs)
		goto reclaim_init_fail;

	for (i = 0; i < vcpu_len; i++) {
		init_emp_list(&inactive->mru_bufs[i]);
		init_emp_list(&inactive->lru_bufs[i]);
	}

skip_alloc_bufs:
#endif /* CONFIG_EMP_VM */
	
	/* initialize per-pcpu proactive_list */
	proactive->host_lru = emp_alloc_pcdata(struct emp_list);
	proactive->host_mru = emp_alloc_pcdata(struct emp_list);
	if (proactive->host_lru == NULL || proactive->host_mru == NULL)
		goto reclaim_init_fail;

	/* pin_list is a part of proactive_list */
	bvma->ftm.pin_list = emp_alloc_pcdata(struct emp_list);
	if (bvma->ftm.pin_list == NULL)
		goto reclaim_init_fail;
	atomic_set(&bvma->ftm.cur_pin_pages, 0);
	atomic_set(&bvma->ftm.num_pin_blocks, 0);
	atomic_set(&bvma->ftm.num_evicted_pin_blocks, 0);

	/* initialize per-pcpu active_list */
	active->host_lru = emp_alloc_pcdata(struct emp_list);
	active->host_mru = emp_alloc_pcdata(struct emp_list);
	if (active->host_lru == NULL || active->host_mru == NULL)
		goto reclaim_init_fail;

	for_each_possible_cpu(cpu) {
		mru = emp_pc_ptr(proactive->host_mru, cpu);
		init_emp_list(mru);
		lru = emp_pc_ptr(proactive->host_lru, cpu);
		init_emp_list(lru);
		pin = emp_pc_ptr(bvma->ftm.pin_list, cpu);
		init_emp_list(pin);

		mru = emp_pc_ptr(active->host_mru, cpu);
		init_emp_list(mru);
		lru = emp_pc_ptr(active->host_lru, cpu);
		init_emp_list(lru);
	}

	/* initialize per-pcpu inactive_list
	 * host_mru holds inactive pages; host_lru holds writeback pages */
	inactive->host_mru = emp_alloc_pcdata(struct emp_list);
	inactive->host_lru = emp_alloc_pcdata(struct emp_list);
	if (inactive->host_mru == NULL || inactive->host_lru == NULL)
		goto reclaim_init_fail;

	for_each_possible_cpu(cpu) {
		mru = emp_pc_ptr(inactive->host_mru, cpu);
		init_emp_list(mru);
		lru = emp_pc_ptr(inactive->host_lru, cpu);
		init_emp_list(lru);
	}

	return 0;

reclaim_init_fail:
#ifdef CONFIG_EMP_VM
	if (inactive->mru_bufs) {
		emp_kfree(inactive->mru_bufs);
		inactive->mru_bufs = NULL;
	}
	if (inactive->lru_bufs) {
		emp_kfree(inactive->lru_bufs);
		inactive->lru_bufs = NULL;
	}
	if (active->mru_bufs) {
		emp_kfree(active->mru_bufs);
		active->mru_bufs = NULL;
	}
	if (active->lru_bufs) {
		emp_kfree(active->lru_bufs);
		active->lru_bufs = NULL;
	}
	if (proactive->mru_bufs) {
		emp_kfree(proactive->mru_bufs);
		proactive->mru_bufs = NULL;
	}
	if (proactive->lru_bufs) {
		emp_kfree(proactive->lru_bufs);
		proactive->lru_bufs = NULL;
	}
#endif /* CONFIG_EMP_VM */
	if ((inactive->host_mru)) {
		emp_free_pcdata(inactive->host_mru);
		inactive->host_mru = NULL;
	}
	if ((inactive->host_lru)) {
		emp_free_pcdata(inactive->host_lru);
		inactive->host_lru = NULL;
	}
	if ((active->host_mru)) {
		emp_free_pcdata(active->host_mru);
		active->host_mru = NULL;
	}
	if ((active->host_lru)) {
		emp_free_pcdata(active->host_lru);
		active->host_lru = NULL;
	}
	if (proactive->host_mru) {
		emp_free_pcdata(proactive->host_mru);
		proactive->host_mru = NULL;
	}
	if (proactive->host_lru) {
		emp_free_pcdata(proactive->host_lru);
		proactive->host_lru = NULL;
	}
	if (bvma->ftm.pin_list) {
		emp_free_pcdata(bvma->ftm.pin_list);
		bvma->ftm.pin_list = NULL;
	}
#ifdef CONFIG_EMP_VM
#ifdef CONFIG_EMP_DEBUG
	proactive->abuf_len = 0;
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
	struct slru *proactive = &emm->ftm.proactive_list;
	struct slru *active = &emm->ftm.active_list;
	struct slru *inactive = &emm->ftm.inactive_list;

	debug_reclaim_exit_proactive(emm, proactive);
	debug_reclaim_exit_active(emm, active);
	debug_reclaim_exit_inactive(emm, inactive);

#ifdef CONFIG_EMP_VM
	if (inactive->mru_bufs) {
		emp_kfree(inactive->mru_bufs);
		inactive->mru_bufs = NULL;
	}
	if (inactive->lru_bufs) {
		emp_kfree(inactive->lru_bufs);
		inactive->lru_bufs = NULL;
	}
	if (active->mru_bufs) {
		emp_kfree(active->mru_bufs);
		active->mru_bufs = NULL;
	}
	if (active->lru_bufs) {
		emp_kfree(active->lru_bufs);
		active->lru_bufs = NULL;
	}
	if (proactive->mru_bufs) {
		emp_kfree(proactive->mru_bufs);
		proactive->mru_bufs = NULL;
	}
	if (proactive->lru_bufs) {
		emp_kfree(proactive->lru_bufs);
		proactive->lru_bufs = NULL;
	}
#endif /* CONFIG_EMP_VM */

	if (inactive->host_mru) {
		emp_free_pcdata(inactive->host_mru);
		inactive->host_mru = NULL;
	}
	if (inactive->host_lru) {
		emp_free_pcdata(inactive->host_lru);
		inactive->host_lru = NULL;
	}
	if (active->host_mru) {
		emp_free_pcdata(active->host_mru);
		active->host_mru = NULL;
	}
	if (active->host_lru) {
		emp_free_pcdata(active->host_lru);
		active->host_lru = NULL;
	}
	if (proactive->host_mru) {
		emp_free_pcdata(proactive->host_mru);
		proactive->host_mru = NULL;
	}
	if (proactive->host_lru) {
		emp_free_pcdata(proactive->host_lru);
		proactive->host_lru = NULL;
	}
	if (emm->ftm.pin_list) {
		emp_free_pcdata(emm->ftm.pin_list);
		emm->ftm.pin_list = NULL;
	}
#ifdef CONFIG_EMP_VM
#ifdef CONFIG_EMP_DEBUG
	proactive->abuf_len = 0;
	active->abuf_len = 0;
	inactive->abuf_len = 0;
#endif /* CONFIG_EMP_DEBUG */
#endif /* CONFIG_EMP_VM */
}

