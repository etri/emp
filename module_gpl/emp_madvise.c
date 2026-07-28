#include "vm.h"
#include "page_mgmt.h"
#include "pcalloc.h"
#include "block.h"
#include "reclaim.h"
#include "paging.h"
#include "vcpu_var.h"
#include "emp_madvise.h"

#define LEN_FLUSH_HEADS NUM_VICTIM_CLUSTER
struct blks_ctx {
	struct vcpu_var *cpu; // current cpu
	struct emp_gpa *heads[LEN_FLUSH_HEADS];
	int num_heads;
	int size_heads;
#ifdef CONFIG_EMP_DEBUG
	struct debug_blks_ctx_stat {
		union {
			struct {
				unsigned long prefetch_called;
				unsigned long prefetch_try;
				unsigned long prefetch_trylock_failed;
				unsigned long prefetch_prefetched;
				unsigned long prefetch_prefetch_once;
				unsigned long prefetch_iomask;
				unsigned long prefetch_stale;
				unsigned long prefetch_promote;
				unsigned long prefetch_remote_failed;
				unsigned long prefetch_remote_succeed;
				unsigned long prefetch_active;
				unsigned long prefetch_inactive;
				unsigned long prefetch_wb;
				unsigned long complete_called;
				unsigned long complete_try;
				unsigned long complete_lock_failed;
				unsigned long complete_prefetched;
				unsigned long complete_remote_failed;
				unsigned long complete_remote_succeed;
			};
			struct {
				unsigned long dontneed_called;
				unsigned long dontneed_try;
				unsigned long dontneed_trylock_failed;
				unsigned long dontneed_prefetched;
				unsigned long dontneed_succeed;
				unsigned long dontneed_active;
				unsigned long dontneed_inactive;
				unsigned long dontneed_wb;
				unsigned long dontneed_remote;
			};
		};
	} stat;
#endif
};

#ifdef CONFIG_EMP_DEBUG
#define debug_blks_ctx_inc(ctx, field) do { (ctx)->stat.field += 1; } while (0)
#define debug_blks_ctx_init(ctx) do { \
	memset(&ctx->stat, 0, sizeof(struct debug_blks_ctx_stat)); \
} while (0)
#else
#define debug_blks_ctx_inc(ctx, field) do {} while (0)
#define debug_blks_ctx_init(ctx) do {} while (0)
#endif


static inline void
blks_ctx_init(struct emp_mm *emm, struct blks_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->cpu = emp_this_cpu_ptr(emm->pcpus);
	debug_blks_ctx_init(ctx);
}

static inline void
blks_ctx_add_head(struct blks_ctx *ctx, struct emp_gpa *head)
{
	debug_assert(ctx->num_heads < LEN_FLUSH_HEADS);
	ctx->heads[ctx->num_heads] = head;
	ctx->num_heads++;
	ctx->size_heads += gpa_block_size(head);
}

static inline int
blks_ctx_flush_prefetch(struct emp_mm *emm, struct blks_ctx *ctx, bool force)
{
	if (ctx->num_heads >= LEN_FLUSH_HEADS || force) {
		int i, ret;
		/* To indicate the call from blks_ctx_flush,
		 * the last parameter should be negative. */
		ret = update_lru_lists(emm, ctx->cpu, ctx->heads,
					ctx->num_heads, -ctx->size_heads);
		for (i = 0; i < ctx->num_heads; i++)
			emp_unlock_block(ctx->heads[i]);
		ctx->num_heads = 0;
		ctx->size_heads = 0;
		return ret;
	} else
		return 0;
}

static int handle_remote_prefetch(struct emp_mm *emm, struct emp_vmr *vmr,
				struct emp_gpa *head, unsigned long idx,
				struct blks_ctx *ctx) {
	int ret;
	ret = fetch_block(emm, vmr, head, idx, 0, ctx->cpu,
					false, false, false);
	debug_progress(head, ret);
	if (unlikely(ret < 0))
		return ret;
	set_gpa_flags_if_unset(head, GPA_PREFETCHED_BLK_MASK);	
	set_gpa_flags_if_unset(head, GPA_PREFETCH_ONCE_MASK);
	blks_ctx_add_head(ctx, head);
	return ret;
}

/**
 * __emp_blk_prefetch: block-prefetch a specific address 
 * @param ???
 *
 * @retval n: gpa_block_size()
 * @retval 0: gpa is null
 * @retval -n: Error
 */
static int __emp_blk_prefetch(struct emp_mm *emm, struct emp_vmr *vmr,
			unsigned long idx, struct blks_ctx *ctx)
{
	struct emp_gpa *gpa, *head;
	int r, ret;

	debug_blks_ctx_inc(ctx, prefetch_called);
	gpa = raw_get_gpadesc(vmr, idx);
	if (!gpa)
		return 0;

	emp_stat_inc(emm, blk_prefetch_try);
	debug_blks_ctx_inc(ctx, prefetch_try);

	head = emp_trylock_block(vmr, &gpa, idx);
	if (head == NULL) {
		debug_blks_ctx_inc(ctx, prefetch_trylock_failed);
		return gpa_block_size(gpa);
	}

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
	if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK)) {
		debug_blks_ctx_inc(ctx, prefetch_prefetched);
		goto set_promote;
	}
#endif /* CONFIG_EMP_BLOCK */
	
	if (is_gpa_flags_set(head, GPA_PREFETCH_ONCE_MASK)) {
		debug_blks_ctx_inc(ctx, prefetch_prefetch_once);
		goto set_promote;
	}

#ifdef CONFIG_EMP_IO
	if (is_gpa_flags_set(head, GPA_IO_MASK)) {
		debug_blks_ctx_inc(ctx, prefetch_iomask);
		goto unlock;
	}
#endif
#ifdef CONFIG_EMP_OPT
	if (is_gpa_flags_set(head, GPA_STALE_BLOCK_MASK)) {
		debug_blks_ctx_inc(ctx, prefetch_stale);
		goto unlock;
	}
#endif

	/* Prevent the duplicated promotion */
	if (is_gpa_flags_set(head, GPA_PROMOTE_MASK)) {
		debug_blks_ctx_inc(ctx, prefetch_promote);
		goto unlock;
	}

	if (head->r_state == GPA_INIT) {
		emp_stat_inc(emm, blk_prefetch_remote);
		r = handle_remote_prefetch(emm, vmr, head, idx, ctx);
		debug_progress(head, r);
		if (r < 0) {
			debug_blks_ctx_inc(ctx, prefetch_remote_failed);
			ret = r;
			goto unlock;
		}
		debug_blks_ctx_inc(ctx, prefetch_remote_succeed);
		head->r_state = GPA_ACTIVE;
		set_gpa_flags_if_unset(head, GPA_HPT_MASK);

		r = blks_ctx_flush_prefetch(emm, ctx, false);
		if (r < 0)
			ret = r;
		return ret;
	}

set_promote:
	/* For the local block, promotion is enough */
	set_gpa_flags_if_unset(head, GPA_PROMOTE_MASK);

	if (head->r_state == GPA_ACTIVE) {
		emp_stat_inc(emm, blk_prefetch_active);
		debug_blks_ctx_inc(ctx, prefetch_active);
	} else if (head->r_state == GPA_INACTIVE) {
		emp_stat_inc(emm, blk_prefetch_inactive);
		debug_blks_ctx_inc(ctx, prefetch_inactive);
	} else if (head->r_state == GPA_WB) {
		emp_stat_inc(emm, blk_prefetch_writeback);
		debug_blks_ctx_inc(ctx, prefetch_wb);
	}

unlock:
	emp_unlock_block(head);
	return ret;
}

/**
 * __emp_blk_prefetch_complete: complete the prefetching
 * @param ???
 *
 * @retval n: gpa_block_size()
 * @retval 0: gpa is null
 * @retval -n: Error
 */
static int __emp_blk_prefetch_complete(struct emp_mm *emm, struct emp_vmr *vmr,
				unsigned long idx, struct blks_ctx *ctx)
{
	struct emp_gpa *gpa, *head;
	int r, ret;

	debug_blks_ctx_inc(ctx, complete_called);
	gpa = raw_get_gpadesc(vmr, idx);
	if (!gpa)
		return 0;
	debug_blks_ctx_inc(ctx, complete_try);

	/* We don't use trylock to assure that all blocks are prefetched */
	head = emp_lock_block(vmr, &gpa, idx);
	if (head == NULL) {
		debug_blks_ctx_inc(ctx, complete_lock_failed);
		return gpa_block_size(gpa);
	}

	if (likely(gpa == head)) {
		ret = gpa_block_size(gpa);
	} else {
		debug_assert(head < gpa);
		ret = gpa_block_size(gpa) - gpa_subblock_size(gpa) * (gpa - head);
		idx = idx - (gpa - head);
	}
	debug_progress(head, head->r_state);
	debug_progress(head, __get_gpa_flags(head));

	/* Wait and map the prefetched page to prevent minor faults */
#ifdef CONFIG_EMP_BLOCK
	if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK)) {
		debug_blks_ctx_inc(ctx, complete_prefetched);
		if (is_gpa_flags_set(head, GPA_HPT_MASK))
			emm->vops.clear_gpa_prefetched_hpt(emm, vmr, head, idx);
#ifdef CONFIG_EMP_VM
		if (is_gpa_flags_set(head, GPA_EPT_MASK)) {
			/* TODO: assure to map EPT */
		}
#endif /* CONFIG_EMP_VM */
		clear_gpa_flags_if_set(head, GPA_PREFETCHED_MASK);
	}
#endif /* CONFIG_EMP_BLOCK */

	/* Assure that all blocks are fetched */
	if (head->r_state == GPA_INIT) {
		r = handle_remote_prefetch(emm, vmr, head, idx, ctx);
		debug_progress(head, r);
		if (r < 0) {
			debug_blks_ctx_inc(ctx, complete_remote_failed);
			ret = r;
			goto unlock;
		}
		debug_blks_ctx_inc(ctx, complete_remote_succeed);
		head->r_state = GPA_ACTIVE;
		set_gpa_flags_if_unset(head, GPA_PROMOTE_MASK);

		r = blks_ctx_flush_prefetch(emm, ctx, false);
		if (r < 0)
			ret = r;
		return ret;
	}

	/* Assure setting the promote mask */
	set_gpa_flags_if_unset(head, GPA_PROMOTE_MASK);

unlock:
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

// for MADV_EMP_WILLNEED
long emp_blk_prefetch(struct emp_mm *emm, unsigned long addr, unsigned long __size)
{
	unsigned long size, addr_start, addr_end, vmr_addr_end;
	struct emp_vmr *vmr;
	int sb_order = bvma_subblock_order(emm), order;
	unsigned long idx;
	int block_size;
	struct blks_ctx ctx;
	bool force;
	long ret;

	if (__size >= 0) {
		size = __size;
		force = false;
	} else {
		size = -__size;
		force = true;
	}

	blks_ctx_init(emm, &ctx);

	if ((addr & (PAGE_SIZE - 1)) != 0) {
		size += addr & ~PAGE_MASK;
		addr = addr & PAGE_MASK;
	}
	addr_start = addr;
	addr_end = addr + size;

	while (addr < addr_end) {
		/* returns error for invalid HVAs */
		if ((vmr = emp_vmr_lookup_hva(emm, addr)) == NULL)
			return -EINVAL;
		vmr_addr_end = vmr->vm_end < addr_end ? vmr->vm_end : addr_end;
		idx = (addr - vmr->descs->vm_base) >> (PAGE_SHIFT + sb_order);

		while (addr < vmr_addr_end) {
			block_size = __emp_blk_prefetch(emm, vmr, idx, &ctx);
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

	ret = (long) blks_ctx_flush_prefetch(emm, &ctx, true);
	dprintk("[BLK_PREFETCH] addr: %ld size: %ld force: %d (STAT) called: %ld try: %ld trylock_failed: %ld prefetched: %ld prefetch_once: %ld iomask: %ld stale: %ld promote: %ld remote_fail: %ld remote_succeed: %ld active: %ld inactive: %ld wb: %ld\n",
			addr_start, size, force,
			ctx.stat.prefetch_called,
			ctx.stat.prefetch_try,
			ctx.stat.prefetch_trylock_failed,
			ctx.stat.prefetch_prefetched,
			ctx.stat.prefetch_prefetch_once,
			ctx.stat.prefetch_iomask,
			ctx.stat.prefetch_stale,
			ctx.stat.prefetch_promote,
			ctx.stat.prefetch_remote_failed,
			ctx.stat.prefetch_remote_succeed,
			ctx.stat.prefetch_active,
			ctx.stat.prefetch_inactive,
			ctx.stat.prefetch_wb);
	if (!force)
		return ret;

	addr = addr_start;
	while (addr < addr_end) {
		/* returns error for invalid HVAs */
		if ((vmr = emp_vmr_lookup_hva(emm, addr)) == NULL)
			return -EINVAL;
		vmr_addr_end = vmr->vm_end < addr_end ? vmr->vm_end : addr_end;
		idx = (addr - vmr->descs->vm_base) >> (PAGE_SHIFT + sb_order);

		while (addr < vmr_addr_end) {
			block_size = __emp_blk_prefetch_complete(emm, vmr, idx, &ctx);
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

	ret = (long) blks_ctx_flush_prefetch(emm, &ctx, true);
	dprintk("[BLK_PREFETCH_FORCE] addr: %ld size: %ld force: %d (STAT) called: %ld try: %ld lock_failed: %ld prefetched: %ld remote_fail: %ld remote_succeed: %ld\n",
			addr_start, size, force,
			ctx.stat.complete_called,
			ctx.stat.complete_try,
			ctx.stat.complete_lock_failed,
			ctx.stat.complete_prefetched,
			ctx.stat.complete_remote_failed,
			ctx.stat.complete_remote_succeed);
	return ret;
}
