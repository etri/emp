#include "vm.h"
#include "page_mgmt.h"
#include "pcalloc.h"
#include "block.h"
#include "reclaim.h"
#include "paging.h"

static int handle_remote_prefetch(struct emp_mm *emm, struct emp_vmr *vmr,
				struct emp_gpa *head, unsigned long idx) {
	struct vcpu_var *cpu;
	int ret;
	cpu = emp_this_cpu_ptr(emm->pcpus);
	ret = fetch_block(emm, vmr, head, idx, 0, cpu, false, false, false);
	debug_progress(head, ret);
	if (unlikely(ret < 0))
		return ret;
	set_gpa_flags_if_unset(head, GPA_PREFETCHED_BLK_MASK);	
	set_gpa_flags_if_unset(head, GPA_PREFETCH_ONCE_MASK);
	return update_lru_lists(emm, cpu, &head, 1, gpa_block_size(head)); 
}

/**
 * __emp_blk_prefetch: block-prefetch a specific address 
 * @param ???
 *
 * Set @block_size as gpa_block_size() or 0 if gpa is null
 *
 * @retval 1: prefetch is done
 * @retval 0: prefetch is skipped
 * @retval -n: Error
 */
int __emp_blk_prefetch(struct emp_mm *emm, struct emp_vmr *vmr, unsigned long idx)
{
	struct emp_gpa *gpa, *head;
	int r, ret = 0;

	gpa = raw_get_gpadesc(vmr, idx);
	if (!gpa)
		return 0;

#ifdef CONFIG_EMP_STAT
	emm->stat.blk_prefetch_try++;
#endif

	head = emp_trylock_block(vmr, &gpa, idx);
	if (head == NULL)
		return gpa_block_size(gpa);

	if (likely(gpa == head)) {
		ret = gpa_block_size(gpa);
	} else {
		debug_assert(head < gpa);
		ret = gpa_block_size(gpa) - gpa_subblock_size(gpa) * (gpa - head);
		idx = idx - (gpa - head);
	}
	debug_progress(head, head->r_state);
	debug_progress(head, __get_gpa_flags(head));

#ifdef CONFIG_EMP_BLOCK
	/* Already prefetched, even if CSF or CPF */
	if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK))
		goto out;
#endif /* CONFIG_EMP_BLOCK */
	
	if (is_gpa_flags_set(head, GPA_PREFETCH_ONCE_MASK))
		goto out;

#ifdef CONFIG_EMP_IO
	if (is_gpa_flags_set(head, GPA_IO_MASK))
		goto out;
#endif
#ifdef CONFIG_EMP_OPT
	if (is_gpa_flags_set(head, GPA_STALE_BLOCK_MASK))
		goto out;
#endif

	if (head->r_state != GPA_INIT) {
		/* TODO: implement blk_prefetch for local blocks.
		 * 1) GPA_ACTIVE: it's already active. Just go out.
		 * 2) GPA_INACTIVE: move to active.
		 * 3) GPA_WB: Set a newly defined flag.
		 *            When writeback_fault, clear the flag and go on.
		 *            When pop_writeback_request, clear the flag and move to active.
		 * 4) GPA_FETCHING: never happened. This is a hidden state.
		 */
#ifdef CONFIG_EMP_STAT
		if (head->r_state == GPA_ACTIVE)
			emm->stat.blk_prefetch_active++;
		if (head->r_state == GPA_INACTIVE)
			emm->stat.blk_prefetch_inactive++;
		else if (head->r_state == GPA_WB)
			emm->stat.blk_prefetch_writeback++;
#endif
		goto out;
	} else {
#ifdef CONFIG_EMP_STAT
		emm->stat.blk_prefetch_remote++;
#endif
		r = handle_remote_prefetch(emm, vmr, head, idx);
		debug_progress(head, r);
		if (r < 0) {
			ret = r;
			goto out;
		}
	}

	head->r_state = GPA_ACTIVE;
	set_gpa_flags_if_unset(head, GPA_HPT_MASK);
out:
	emp_unlock_block(head);
	return ret;
}

static inline int __get_max_block_order(struct emp_vmr *vmr, unsigned long idx) {
	struct gpadesc_region *region;
	region = get_gpadesc_region(vmr->descs, idx);
	/* NOTE: idx should reside in the vmr */
	debug_assert(region != NULL);
	return region->block_order;
}

static inline int __get_max_block_size(struct emp_vmr *vmr, unsigned long idx) {
	return 1 << __get_max_block_order(vmr, idx);
}

long emp_blk_prefetch(struct emp_mm *emm, unsigned long addr, unsigned long size)
{
	unsigned long addr_end, vmr_addr_end;
	struct emp_vmr *vmr;
	int sb_order = bvma_subblock_order(emm), order;
	unsigned long idx;
	int block_size;

	if ((addr & (PAGE_SIZE - 1)) != 0) {
		size += addr & ~PAGE_MASK;
		addr = addr & PAGE_MASK;
	}
	addr_end = addr + size;
	
	while (addr < addr_end) {
		/* returns error for invalid HVAs */
		if ((vmr = emp_vmr_lookup_hva(emm, addr)) == NULL)
			return -EINVAL;
		vmr_addr_end = vmr->vm_end < addr_end ? vmr->vm_end : addr_end;
		idx = (addr - vmr->descs->vm_base) >> (PAGE_SHIFT + sb_order);

		while (addr < vmr_addr_end) {
			block_size = __emp_blk_prefetch(emm, vmr, idx);
			if (unlikely(block_size < 0))
				return (long) block_size;
			if (block_size > 0) {
				addr += block_size << PAGE_SHIFT;
				idx += block_size >> sb_order;
				continue;
			}
			order = __get_max_block_order(vmr, idx) - sb_order;
			if (idx == _emp_get_block_head_index(vmr, idx, order)) {
				// aligned to max_block
				addr += 1UL << (order + sb_order + PAGE_SHIFT);
				idx += 1UL << order;
			} else {
				// subblock-level skipping
				addr += 1UL << (sb_order + PAGE_SHIFT);
				idx++;
			}
		}
	}

	return 0;
}
