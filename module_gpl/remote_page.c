#include <linux/list.h>
#include "config.h"
#include "vm.h"
#include "remote_page.h"
#include "block.h"
#include "debug.h"
#ifdef CONFIG_EMP_USER
#include "cow.h"
#endif
#include "udma.h"

/**
 * __remote_page_release - release a remote page and insert to bitmap
 * @param emm emp_mm data structure
 * @param rp remote page structure to release
 *
 * return the number of remote pages released.
 */
static inline unsigned long __remote_page_release(struct emp_mm *emm, struct remote_page *rp)
{
	struct memreg *mr = emm->mrs.memregs[get_remote_page_mrid(rp)];
	unsigned long pgoff = get_remote_page_offset(rp) >> mr->dma_order;
	unsigned long dma_size = 1 << mr->dma_order;

	debug_assert(is_remote_page_cow(rp) == false);
	debug_assert(is_remote_page_free(rp) == false);

	spin_lock(&mr->lock);
	atomic64_sub(dma_size, &mr->alloc_len);
	__set_bit(pgoff, mr->alloc_bitmap);

	if (pgoff < mr->free_start)
		mr->free_start = pgoff;
	spin_unlock(&mr->lock);
	return dma_size;
}

static inline void
____free_remote_page(struct emp_mm *emm, struct remote_page *remote_page)
{
	if (!emm->config.remote_reuse) {
		__remote_page_release(emm, remote_page);
		set_remote_page_free(remote_page);
	}
}

void __free_remote_page(struct emp_mm *emm, struct remote_page *remote_page)
{
	____free_remote_page(emm, remote_page);
}
EXPORT_SYMBOL(__free_remote_page);

static void __free_remote_page_block(struct emp_mm *emm, struct emp_gpa *head)
{
	struct emp_gpa *gpa;
	for_each_gpas(gpa, head) {
		if (is_gpa_remote_page_free(gpa))
			continue;
		____free_remote_page(emm, &gpa->remote_page);
	}
}

/**
 * free_remote_page - Free a remote page and insert it to a free remote page list
 * @param emm emp_mm data structure
 * @param remote_page remote page info
 * @parem do_free do free the remote. If freeing should be delayed, set this false.
 *
 * @retval the remote page is freed or not.
 *
 * If do_free is not set, the remote page is not actually freed even if this returns true.
 */
bool free_remote_page(struct emp_mm *emm,
		      struct emp_gpa *gpa, bool do_free)
{
	struct remote_page *remote_page = &gpa->remote_page;
	if (debug_WARN_ONCE(is_remote_page_free(remote_page),
				"%s: remote page already freed. do_free: %c\n",
				__func__, do_free ? 1 : 0))
		return false;


	/* if some other process share this remote page, don't free it */
	if (is_remote_page_cow(remote_page))
		return false;

	if (do_free)
		____free_remote_page(emm, remote_page);
	return true;
}
EXPORT_SYMBOL(free_remote_page);

static inline bool __check_mr_change(struct emp_mm *emm)
{
	if (emm->config.remote_policy_block == 0)
		return false;
	if (atomic64_fetch_add(1, &emm->mrs.memregs_num_alloc)
				% emm->config.remote_policy_block == 0)
		return true;
	else
		return false;
}

static inline struct memreg *
__lock_next_avail_mr(struct emp_mm *emm, struct memreg *prev,
					int mrid_first, int num)
{
	struct memreg *mr;
	int mrid_last = emm->mrs.memregs_last;
	int mrid = prev ? ((prev->id + 1) % mrid_last) : mrid_first;

	if (unlikely(mrid_last == 0))
		return NULL;

	do {
		mr = emm->mrs.memregs[mrid];
		if (mr && mr->size >= atomic64_read(&mr->alloc_len) + num) {
			spin_lock(&mr->lock);
			if (mr->size >= atomic64_read(&mr->alloc_len) + num)
				return mr;
			spin_unlock(&mr->lock);
		}
		mrid = (mrid + 1) % mrid_last;
	} while (mrid != mrid_first);

	return NULL;
}

// Return the number of gpas that failed to allocate remote page
static int remote_page_policy_sequential(struct emp_mm *emm,
					struct emp_gpa *head, int mrid_first)
{
	int sb_order = gpa_subblock_order(head);
	unsigned long offset;
	struct emp_gpa *gpa = head;
	struct memreg *mr = NULL;
	int rem = num_subblock_in_block(head);

	mr = __lock_next_avail_mr(emm, mr, mrid_first, 1 << sb_order);

	while (mr && rem > 0) {
		if (unlikely(!is_gpa_remote_page_free(gpa)))
			goto next_gpa;
		// Increase alloc_len first to reduce the locking in
		// __lock_next_avail_mr()
		atomic64_add(1 << sb_order, &mr->alloc_len);
		offset = find_next_bit(mr->alloc_bitmap, mr->size >> sb_order,
						mr->alloc_next);
		if (offset == mr->size >> sb_order)
			offset = find_next_bit(mr->alloc_bitmap, mr->size >> sb_order,
							mr->free_start);
		if (unlikely(offset == mr->size >> sb_order)) {
			atomic64_sub(1 << sb_order, &mr->alloc_len);
			spin_unlock(&mr->lock);
			mr = __lock_next_avail_mr(emm, mr, mrid_first, 1 << sb_order);
			continue;
		}

		__clear_bit(offset, mr->alloc_bitmap);
		mr->alloc_next = offset + 1;
		if (mr->free_start == offset)
			mr->free_start = offset + 1;

		set_gpa_remote_page(gpa,
			make_remote_page_val(mr->id, offset << sb_order));
next_gpa:
		gpa++;
		rem--;
	}

	if (likely(mr)) {
		atomic_set(&emm->mrs.memregs_last_alloc, mr->id);
		spin_unlock(&mr->lock);
	}

	return rem;
}

// Return the number of gpas that failed to allocate remote page
static int remote_page_policy_same_mr(struct emp_mm *emm,
					struct emp_gpa *head, int mrid_first)
{
	int sb_order = gpa_subblock_order(head);
	unsigned long offset;
	struct emp_gpa *gpa = head;
	struct memreg *mr = NULL;
	int rem = num_subblock_in_block(head);

	mr = __lock_next_avail_mr(emm, NULL, mrid_first, rem << sb_order);
	if (unlikely(mr == NULL))
		return rem;

	// Increase alloc_len first to reduce the locking in
	// __lock_next_avail_mr()
	atomic64_add(rem << sb_order, &mr->alloc_len);
	// Early update of memregs_last_alloc
	atomic_set(&emm->mrs.memregs_last_alloc, mr->id);

	for_each_gpas(gpa, head) {
		if (unlikely(!is_gpa_remote_page_free(gpa)))
			continue;
		offset = find_next_bit(mr->alloc_bitmap, mr->size >> sb_order,
						mr->alloc_next);
		if (offset == mr->size >> sb_order)
			offset = find_next_bit(mr->alloc_bitmap, mr->size >> sb_order,
							mr->free_start);
		// We checked that there are enough subblocks and mr is locked
		debug_BUG_ON(offset == mr->size >> sb_order);

		__clear_bit(offset, mr->alloc_bitmap);
		mr->alloc_next = offset + 1;
		if (mr->free_start == offset)
			mr->free_start = offset + 1;

		set_gpa_remote_page(gpa,
			make_remote_page_val(mr->id, offset << sb_order));
	}

	spin_unlock(&mr->lock);
	return 0;
}

// Return the number of gpas that failed to allocate remote page
static int remote_page_policy_diff_mr(struct emp_mm *emm,
					struct emp_gpa *head, int mrid_first)
{
	int sb_order = gpa_subblock_order(head);
	unsigned long offset;
	struct emp_gpa *gpa = head;
	struct memreg *mr;
	int mrid_last = emm->mrs.memregs_last;
	int rem = num_subblock_in_block(head);

	mr = __lock_next_avail_mr(emm, NULL, mrid_first, 1 << sb_order);

	while (mr && rem > 0) {
		if (unlikely(!is_gpa_remote_page_free(gpa)))
			goto next_gpa;
		// Increase alloc_len first to reduce the locking in
		// __lock_next_avail_mr()
		atomic64_add(1 << sb_order, &mr->alloc_len);
		offset = find_next_bit(mr->alloc_bitmap, mr->size >> sb_order,
						mr->alloc_next);
		if (offset == mr->size >> sb_order)
			offset = find_next_bit(mr->alloc_bitmap, mr->size >> sb_order,
							mr->free_start);
		debug_BUG_ON(offset == mr->size >> sb_order);

		__clear_bit(offset, mr->alloc_bitmap);
		mr->alloc_next = offset + 1;
		if (mr->free_start == offset)
			mr->free_start = offset + 1;

		set_gpa_remote_page(gpa,
			make_remote_page_val(mr->id, offset << sb_order));
next_gpa:
		gpa++;
		rem--;

		// go to the next mr
		mrid_first = (mr->id + 1) % mrid_last;
		if (mrid_first != mr->id) {
			spin_unlock(&mr->lock);
			mr = __lock_next_avail_mr(emm, NULL, mrid_first, 1);
		}
	}

	if (likely(mr)) {
		atomic_set(&emm->mrs.memregs_last_alloc, mr->id);
		spin_unlock(&mr->lock);
	}

	return rem;
}

typedef int (*remote_page_policy_t)(struct emp_mm *emm,
				struct emp_gpa *head, int mrid_first);
remote_page_policy_t remote_page_policy[NUM_REMOTE_POLICY_SUBBLOCK] = {
	remote_page_policy_sequential,
	remote_page_policy_same_mr,
	remote_page_policy_diff_mr
};

/**
 * __alloc_remote_page - Alloc a remote page for a block
 * @param bvma bvma data structure
 * @param head the head of a gpa block
 *
 * @retval true: Success
 * @retval false: Error
 */
static inline bool __alloc_remote_page(struct emp_mm *emm, struct emp_gpa *head)
{
	int mrid_first;
	int policy;

	// Change the memory region for every remote_policy_block times of allocations
	mrid_first = atomic_read(&emm->mrs.memregs_last_alloc);
	if (__check_mr_change(emm))
		mrid_first = (mrid_first + 1) % emm->mrs.memregs_last;

	policy = emm->config.remote_policy_subblock;
	debug_BUG_ON(policy >= NUM_REMOTE_POLICY_SUBBLOCK);
	if (likely(remote_page_policy[policy](emm, head, mrid_first) == 0))
		return true;
	__free_remote_page_block(emm, head);
	return false;
}

/**
 * alloc_remote_page - Alloc a remote page for a block
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 * @param head the head of a gpa block
 *
 * @retval true: Success
 * @retval false: Error
 */
bool alloc_remote_page(struct emp_mm *emm, struct emp_gpa *head)
{
	struct emp_gpa *gpa;
	debug_alloc_remote_page(emm, head);

	for_each_gpas(gpa, head) {
		if (is_gpa_remote_page_free(gpa))
			goto alloc;
	}
	return true;

alloc:
	if (unlikely(!__alloc_remote_page(emm, head)))
		goto error;

	debug_alloc_remote_page2(emm, head);
	return true;

error:
	for_each_gpas(gpa, head) {
		if (is_gpa_remote_page_free(gpa))
			continue;
		____free_remote_page(emm, &gpa->remote_page);
	}
	return false;
}
EXPORT_SYMBOL(alloc_remote_page);

/**
 * remote_page_release - release the number of remote pages
 * @emm emp mm structure
 * @head max block head
 * @num the number of gpa descriptors that belongs to @head.
 */
inline void COMPILER_DEBUG
remote_page_release(struct emp_mm *emm, struct emp_gpa *head, int num)
{
	struct emp_gpa *g;
	int i;

	for (i = 0, g = head; i < num; i++, g++) {
		if (is_gpa_remote_page_free(g))
			continue;
#ifdef CONFIG_EMP_USER
		/* we do not use the remote page anymore */
		if (put_cow_remote_page(emm, g) == false)
			continue;
#endif
		__remote_page_release(emm, &g->remote_page);
	}
}

void adjust_remote_page_policy(struct emp_mm *emm, struct memreg *mr)
{
#ifdef CONFIG_EMP_RDMA
	if (mr->type == MR_RDMA && emm->config.remote_policy_subblock
					!= REMOTE_POLICY_SUBBLOCK_SAME_MR) {
		int i;
		for (i = 0; i < N_RDMA_QUEUES; i++) {
			if (mr->conn->contexts[i] == NULL)
				continue;
			if (mr->conn->contexts[i]->chained_ops) {
				printk(KERN_INFO "%s: SUBBLOCK_SAME_MR policy "
					"is enabled since chainged operations "
					"is used\n", __func__);
				emm->config.remote_policy_subblock =
						REMOTE_POLICY_SUBBLOCK_SAME_MR;
				break;
			}
		}
	}
#endif
}

/**
 * remote_page_init - Allocate and initialize the remote pages
 * @param bvma bvma data structure
 *
 * @retval 0: Success
 * @retval n: Error
 */
int remote_page_init(struct emp_mm *bvma)
{
	const int dma_wr_size = 1 << DMA_WRITEBACK_MAX_ORDER;
	int block_order = bvma_subblock_order(bvma);
	if (min((int)(bvma_subblock_size(bvma)), dma_wr_size) >= U8_MAX)
		return -EINVAL;

	bvma->mrs.memregs_block_size = (1 << block_order);
	bvma->mrs.memregs_wdma_size =
	    min((int)(bvma_subblock_size(bvma)), dma_wr_size);
	bvma->mrs.memregs_last = 0;
	atomic64_set(&bvma->mrs.memregs_num_alloc, 0);
	atomic_set(&bvma->mrs.memregs_last_alloc, 0);

	return 0;
}

/**
 * remote_page_exit - Destroy remote page cache
 * @param bvma bvma data structure
 */
void remote_page_exit(struct emp_mm *bvma)
{
}
