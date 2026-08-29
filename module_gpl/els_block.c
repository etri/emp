#include <linux/sched.h>
#include "../module_gpl/reclaim.h"
#include "../module_gpl/block-flag.h"
#include "../module_gpl/donor_mem_rw.h"
#include "../module_gpl/pcalloc.h"
#include "els_block.h"
#include "reclaim.h"
#include "debug.h"
#include "gpa.h"

/**
 * els_get_super_head - Get the head of the stretched elastic block
 * @param bvma bvma data structure
 * @param i gpa of the page
 *
 * @return head of the block
 *
 * It returns the head of the block which contains the elastic block \n
 * It uses to find the head of the block when it will be stretched.
 */
static struct emp_gpa *
els_get_super_head(struct emp_mm *bvma, struct emp_gpa *gpa)
{
	if (gpa_block_order(gpa) >= gpa_max_block_order(gpa))
		return NULL;
	return _emp_get_block_head(gpa, gpa_desc_order(gpa) + 1);
}

static void COMPILER_DEBUG
remove_from_list_except_for_head(struct emp_mm *emm, struct emp_gpa *head) {
	struct slru *proactive = &emm->ftm.proactive_list;
	struct slru *active = &emm->ftm.active_list;
	unsigned long i, size = num_subblock_in_block(head);
	struct emp_gpa *gpa;
	struct local_page *lp;
	int cpu;
	struct emp_list *list;
	for (i = 1, gpa = head + 1; i < size; i++, gpa++) {
		if (!is_local_page_on_list(gpa->local_page))
			continue;

		lp = gpa->local_page;
		cpu = lp->cpu;
		if (clear_gpa_flags_if_set(gpa, GPA_PROACTIVE_MASK)) {
			if (is_local_page_on_mru(lp)) {
				list = get_list_ptr_mru(emm, proactive, cpu);
			} else if (is_local_page_on_lru(lp)) {
				list = get_list_ptr_lru(emm, proactive, cpu);
			} else {
				debug_assert(is_local_page_on_global(lp));
				list = &proactive->list;
			}
		} else {
			if (is_local_page_on_mru(lp)) {
				list = get_list_ptr_mru(emm, active, cpu);
			} else if (is_local_page_on_lru(lp)) {
				list = get_list_ptr_lru(emm, active, cpu);
			} else {
				debug_assert(is_local_page_on_global(lp));
				list = &active->list;
			}
		}

		emp_list_lock(list);
		emp_list_del(&lp->lru_list, list);
		emp_list_unlock(list);
		clear_local_page_list_flags(lp);
	}
}

/**
 * els_update_gpa_flags - Update gpa flags after stretching block
 * @param s next linked list head
 * @param p previous linked list head
 */
static void els_update_gpa_flags(struct emp_gpa *s, struct emp_gpa *p)
{
	debug_els_update_gpa_flags(p);

	if (s != p) { // ==> linked point is changed
		p->r_state = GPA_INIT;

		// set dirty flag according to
		// current and buddy blocks' flags
		set_gpa_flags_if_unset(s, get_gpa_flags(p) & 
				~(GPA_PROACTIVE_MASK | GPA_PROMOTE_MASK));
		// clear dirty and flag of buddy block
		// do not clear GPA_PROACTIVE_MASK since it indicates where the gpa is.
		// GPA_PROACTIVE_MASK will be cleared in remove_from_list_except_for_head().
		clear_gpa_flags_if_set(p, GPA_nPT_MASK | GPA_PROMOTE_MASK |
					GPA_REFERENCED_MASK | GPA_DIRTY_MASK);
	} else { // s == p ==> linked point is not changed
		struct emp_gpa *member;

		member = s + (1 << (gpa_desc_order(s) - 1));
		s->r_state = member->r_state;

		member->r_state = GPA_INIT;
		member->local_page->sptep = NULL;

		// we set dirty flag if buddy block is dirty
		if (clear_gpa_flags_if_set(member, GPA_DIRTY_MASK))
			set_gpa_flags_if_unset(s, GPA_DIRTY_MASK);
	}
}

#ifdef CONFIG_EMP_RDMA
/**
 * els_update_remote_page - Update remote_pages in a stretched block
 * @param bvma bvma data structure
 * @param s head of the stretched block
 */
static void els_update_remote_page(struct emp_mm *bvma, struct emp_gpa *s)
{
	struct emp_gpa *gpa;
	int head_mrid = get_gpa_remote_page_mrid(s);
	bool diff_mr = false;

	if (bvma->config.chained_ops) {
		for_each_gpas(gpa, s) {
			if (get_gpa_remote_page_mrid(gpa) != head_mrid) {
#ifdef CONFIG_EMP_DEBUG
				printk(KERN_ERR "%s stretch blocks with different mrid c: %d, head: %d\n",
						__func__, get_gpa_remote_page_mrid(gpa), head_mrid);
#endif
				diff_mr = true;
				break;
			}
		}
		if (diff_mr)
			remote_page_release(bvma, s, num_subblock_in_block(s));
	}
}
#endif

/**
 * __els_stretch - Stretch the size of a block
 * @param bvma bvma data structure
 * @param s next linked list head
 * @param p previous linked list head
 * @param c current linked list head
 */
static void __els_stretch(struct emp_mm *bvma, struct emp_gpa *s, struct emp_gpa *p, struct emp_gpa *c)
{
	int i, next_order, curr_order;
	unsigned int sb_order;

	sb_order = gpa_subblock_order(s);
	curr_order = gpa_block_order(s);
	next_order = curr_order + 1;
	for (i = 0; i < (1 << (next_order - sb_order)); i++) {
		inc_gpa_block_order(s + i);
		debug___els_stretch(s + i, next_order);
	}

	els_update_gpa_flags(s, p);

	__emp_els_stat_add(bvma, curr_order, block_count, -2);
	__emp_els_stat_add(bvma, next_order, block_count, 1);
	__emp_els_stat_add(bvma, curr_order, block_stretch, 1);
}

/**
 * _els_is_stretchable - Check if the block can be stretched
 * @param shead next head
 * @param chead current head
 * @param state state of the block
 * @param flag flag of the block
 *
 * @return head of buddy block
 *
 * Check the block can be stretched and return the head of the block which will
 * be stretched to the same elastic block
 */
static struct emp_gpa *_els_is_stretchable(struct emp_gpa *shead, struct emp_gpa *chead, int state,
		u32 flag)
{
	struct emp_gpa *buddy;
	bool read_only_mapping;
	int fault_type;
	/* Because max_block_order is 4-bit bit-type, we cannot use min() operator */
	unsigned int max_order = (gpa_max_block_order(shead) > gpa_max_block_order(chead)) ?
				 gpa_max_block_order(chead) : gpa_max_block_order(shead);

#ifdef CONFIG_EMP_VM
#ifdef CONFIG_EMP_DEBUG
	/* If chead is at low memory region, its max block order should be
	 * less then LOW_MEMORY_MAX_ORDER */
	debug_assert((!__is_gpa_flags_set(chead, GPA_LOWMEM_BLOCK_MASK))
			|| gpa_max_block_order(chead) <= LOW_MEMORY_MAX_ORDER);
#endif
#endif

	if (gpa_block_order(shead) >= max_order || gpa_block_order(chead) >= max_order)
		return NULL;

	read_only_mapping = !!(flag & BLOCK_READ_ONLY);
#ifdef CONFIG_EMP_VM
	fault_type = (flag & BLOCK_HVA_FAULT)? GPA_HPT_MASK: GPA_EPT_MASK;
#else
	debug_assert(flag & BLOCK_HVA_FAULT);
	fault_type = GPA_HPT_MASK;
#endif

	buddy = (shead == chead)? (shead + num_subblock_in_block(shead)): shead;
	/* The buddy's own ceiling matters as much as @chead's, because
	 * __els_stretch() raises the block order of every member of the merged
	 * block: a buddy which has already reached its ceiling cannot take
	 * part. A no-op while every gpa in a region shares one ceiling. It is
	 * what will hold a vma split boundary, where the blocks cut to fit it
	 * have their ceiling lowered so they can never be merged back across
	 * it. */
	if (gpa_block_order(buddy) >= gpa_max_block_order(buddy))
		return NULL;
	if (is_gpa_flags_set(buddy, GPA_PREFETCHED_MASK | GPA_PINNED_MASK))
		return NULL;
	if ((buddy->r_state != state) || 
			(gpa_block_order(chead) != gpa_block_order(buddy)) ||
			!__emp_trylock_block(buddy))
		return NULL;
	debug_progress(buddy, chead);

	if ((buddy->r_state != state) ||
			(gpa_block_order(chead) != gpa_block_order(buddy)) ||
			(!!read_only_mapping ==
			 is_gpa_flags_set(buddy, GPA_DIRTY_MASK)) || // ?
			(fault_type != (get_gpa_flags(buddy) & GPA_nPT_MASK)) ||
#ifdef CONFIG_EMP_USER
			/* the fault path reads this flag from the head, so a
			 * merge must not hide two states behind one */
			(((get_gpa_flags(chead) ^ get_gpa_flags(buddy))
					& GPA_WPROTECT_MASK) != 0) ||
#endif
			is_gpa_flags_set(buddy, GPA_STRETCHED_MASK
						| GPA_PREFETCHED_MASK
						| GPA_PINNED_MASK)) {
		debug_BUG_ON(!____emp_gpa_is_locked(buddy));
		debug_progress(buddy, chead);
		__emp_unlock_block(buddy);
		return NULL;
	}

	return buddy;
}

static struct emp_gpa *
els_is_stretchable(struct emp_gpa *shead, struct emp_gpa *chead, u32 flag)
{
	return _els_is_stretchable(shead, chead, GPA_ACTIVE, flag);
}

/**
 * els_stretch_rep - Stretch the elastic block
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param chead current head of block
 * @param flag flag of block
 * @param aggressiveness the level of stretching
 *
 * @retval true: Success
 * @retval false: Error
 */
bool els_stretch_rep(struct emp_mm *bvma, struct vcpu_var *cpu,
		     struct emp_gpa *chead, u32 flag, int aggressiveness) {
	int orig_order, prev_linked_size;
	struct emp_gpa *next_linked, *prev_linked;
	struct emp_gpa *new_primary, *new_secondary;
	int list_type, current_level = 0;
	struct slru *target_list;
	struct emp_vmr *vmr;

	if (bvma->config.els_disabled)
		return false;

	if (flag & BLOCK_NO_FETCH ||
			!(flag & BLOCK_FETCH_IN_PROGRESS))
		return false;

	debug_els_stretch_rep(chead, cpu->id, flag);

	vmr = bvma->vmrs[chead->local_page->vmr_id];
	if ((next_linked = els_get_super_head(bvma, chead)) == NULL)
		return false;

	new_primary = chead;
	orig_order = gpa_block_order(new_primary);
	flag |= IS_IOTHREAD_VCPU(cpu->id)? BLOCK_HVA_FAULT: BLOCK_GPA_FAULT;

	do {
		// we assumes that new_primary must not be linked to lru_chain
		prev_linked = els_is_stretchable(next_linked, new_primary, flag);
		if (prev_linked == NULL)
			break;

		debug_els_stretch_rep2(new_primary, prev_linked);

		if (new_primary < prev_linked) {
			new_secondary = prev_linked;
		} else {
			new_secondary = new_primary;
		}
		prev_linked_size = gpa_block_size(prev_linked);

		// decrease prev_linked's space from target_list
		// it does not belong to target_list
		atomic_sub(prev_linked_size, 
			is_gpa_flags_set(prev_linked, GPA_PROACTIVE_MASK)
				? &bvma->ftm.proactive_list.page_len
				: &bvma->ftm.active_list.page_len);

		__els_stretch(bvma, next_linked, prev_linked, new_primary);

		smp_wmb();
		// release member's lock
		__emp_unlock_block(new_secondary);

		new_primary = next_linked;
		if (aggressiveness && ++current_level >= aggressiveness)
			break;
		next_linked = els_get_super_head(bvma, new_primary);
	} while (next_linked != NULL);

	debug_els_stretch_rep3(new_primary, chead, cpu->id);
	if (orig_order == gpa_block_order(new_primary))
		return false;

#ifdef CONFIG_EMP_RDMA
	els_update_remote_page(bvma, new_primary);
#endif

	// mark the block as stretched so it does not do csf
	set_gpa_flags_if_unset(new_primary, GPA_STRETCHED_MASK);
	// we should prevent prefetch (CSF, CPF)
	// since prefetch mechanisms cannot handle half-fetched and half-non-fetched blocks.
	set_gpa_flags_if_unset(new_primary, GPA_PREFETCH_ONCE_MASK);

	// add a stretched block to the list
	// it will increase gpa_block_size to target_list
	if (is_gpa_flags_set(new_primary, GPA_PROACTIVE_MASK)) {
		list_type = PROACTIVE_LIST;
		target_list = &bvma->ftm.proactive_list;
	} else {
		list_type = ACTIVE_LIST;
		target_list = &bvma->ftm.active_list;
	}

	if (!is_local_page_on_list(new_primary->local_page))
		add_gpas_to_active_list(list_type, bvma, cpu, &new_primary, 1);
	else {
		int new_primary_size = gpa_block_size(new_primary);
		atomic_add(new_primary_size, &target_list->page_len);
	}
	remove_from_list_except_for_head(bvma, new_primary);
	debug_els_stretch_rep4(new_primary);
	debug_els_stretch_rep5(bvma, vmr, new_primary, chead, cpu->id);

	return true;
}

/**
 * els_reduce - Reduce the size of elastic block
 * @param bvma bvma data structure
 * @param s current head of block
 * @param hs divided heads of block
 *
 * @retval true: Success
 * @retval false: Error
 */
bool els_reduce(struct emp_mm *bvma, struct emp_gpa *s, struct emp_gpa *hs[])
{
	struct emp_gpa *buddy, *g;
	int curr_order, next_order;
	unsigned int flag;
	unsigned int sb_order, num_sb;
	
	sb_order = gpa_subblock_order(s);
	num_sb = num_subblock_in_block(s);
	curr_order = gpa_block_order(s);
	next_order = curr_order - 1;
	buddy = s + (1 << (next_order - sb_order));

	if (!__emp_trylock_block(buddy))
		return false;
	debug_progress_start(buddy, s);

	flag = get_gpa_flags(s);

	for (g = s; g < s + num_sb; g++)
		set_gpa_block_order(g, next_order);

	set_gpa_flags(buddy, flag);
	buddy->r_state = s->r_state;
	buddy->local_page->sptep = s->local_page->sptep + ((buddy - s) << sb_order);

	debug___els_reduce(buddy, s);

	hs[0] = s;
	hs[1] = buddy;

	__emp_els_stat_add(bvma, curr_order, block_count, -1);
	__emp_els_stat_add(bvma, next_order, block_count, 2);
	__emp_els_stat_add(bvma, curr_order, block_reduce, 1);

	return true;
}

/**
 * els_tryreduce_complete - Try to reduce the size of elastic block
 * @param bvma bvma data structure
 * @param s current head of block
 * @param hs divided heads of block
 *
 * @retval true: Success
 * @retval false: Error
 */
bool els_tryreduce_complete(struct emp_mm *bvma, struct emp_gpa *s, struct emp_gpa *hs[])
{
	struct emp_gpa *g;
	unsigned int flag;
	int sb_index, num_sb, curr_order;
	int next_order = gpa_subblock_order(s);
	unsigned int sb_order;

	sb_order = gpa_subblock_order(s);
	curr_order = gpa_block_order(s);
	num_sb = num_subblock_in_block(s);

	for_each_gpas(g, s) {
		if (g == s)
			continue;

		if (!__emp_trylock_block(g))
			goto reduce_fail;
		debug_progress_start(g, s);
	}

	flag = get_gpa_flags(s);
	debug___els_tryreduce_complete(s);

	// all the block headers are locked
	for (g = s, sb_index = 0; g < (s + num_sb); g++, sb_index++) {
		// update block_order for every member in the block
		set_gpa_block_order(g, next_order);
		hs[sb_index] = g;
		if (g == s || g->local_page == NULL)
			continue;

		// update flag, r_state for the header of each subblock
		set_gpa_flags(g, flag);
		g->r_state = s->r_state;
		g->local_page->sptep = s->local_page->sptep + ((g - s) << sb_order);
		debug___els_tryreduce_complete2(g, s);
	}


	__emp_els_stat_add(bvma, curr_order, block_count, -1);
	__emp_els_stat_add(bvma, next_order, block_count, sb_index);
	__emp_els_stat_add(bvma, curr_order, block_reduce_complete, 1);

	return true;

reduce_fail:
	{
		struct emp_gpa *e = g;
		for (g = s + 1; g < e; g++)
			__emp_unlock_block(g);
	}
	return false;
}

/**
 * els_init - Initialize elastic block configurations
 * @param bvma bvma data structure
 * @param initial_els_order_max initial value of maximum els block size
 * @param initial_els_disabled Disable the elastic block?
 */
void els_init(struct emp_mm *bvma, bool initial_els_disabled)
{
	bvma->config.els_disabled = initial_els_disabled;

	printk("elastic block: max %d pages. disabled: %d\n",
			(1 << bvma->config.block_order),
			bvma->config.els_disabled);
}
