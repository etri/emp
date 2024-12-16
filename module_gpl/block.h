#ifndef __BLOCK_H__
#define __BLOCK_H__

#include "config.h"
#include "emp_type.h"
#include "gpa.h"
#include "subblock.h"
#include "debug.h"
#include "udma.h"

#ifdef CONFIG_EMP_DEBUG_PROGRESS_GPA_LOCK
#define debug_progress_gpa_lock(gpa, data) debug_progress(gpa, data)
#else
#define debug_progress_gpa_lock(gpa, data) do {} while (0)
#endif

static inline void ____emp_gpa_init_lock(struct emp_gpa *gpa)
{
	atomic_set(&gpa->lock, 0);
}

static inline bool ____emp_gpa_trylock(struct emp_gpa *gpa)
{
	/* if old was 0, it is changed to 1. Thus, we should return 1.
	 * if old was not 0, we should return 0.
	 */
	return !atomic_cmpxchg(&gpa->lock, 0, 1);
}

static inline bool ____emp_gpa_is_locked(struct emp_gpa *gpa)
{
	return atomic_read(&gpa->lock);
}

static inline void ____emp_gpa_unlock(struct emp_gpa *gpa)
{
	atomic_set(&gpa->lock, 0);
}


#define num_subblock_in_block(g) (1)

#define for_each_gpas(pos, head) \
	for (pos = (head); pos != NULL; pos = NULL)

#define for_each_gpas_reverse(pos, head) \
	for (pos = (head); pos != NULL; pos = NULL)

#define for_each_gpas_index(pos, index, head) \
	for (pos = (head), index = 0; pos != NULL; pos = NULL, index++)

#define for_all_gpas_range(vmr, index, pos, start, end) \
	for ((index) = (start), (pos) = get_gpadesc(vmr, index); \
			(pos); \
			(index)++, \
			(pos) = get_gpadesc(vmr, index))

#define raw_for_all_gpas(vmr, index, pos) \
	for ((index) = 0, (pos) = get_next_exist_gpadesc(vmr, &index); \
			(index) < (vmr)->descs->gpa_len; \
			(index)++, \
			(pos) = get_next_exist_gpadesc(vmr, &index))

#define for_all_gpas(vmr, index, pos) \
	for_all_gpas_range(vmr, index, pos, 0, (vmr)->descs->gpa_len)

#define for_all_gpa_heads(vmr, index, pos) for_all_gpas(vmr, index, pos)

#define raw_for_all_gpa_heads(vmr, index, pos) raw_for_all_gpas(vmr, index, pos)

#define gpa_block_order(gpa) (0)
#define gpa_block_size(gpa)	(1UL)
#define __gpa_block_size(gpa, order) (1UL << (order))
#define gpa_next_block(gpa) (gpa + 1)
#define gpa_block_mask(gpa)	(~0UL)
#define gpa_block_offset(gpa, offset) (0)
#define gpa_page_mask(gpa) (~((1UL << PAGE_SHIFT) - 1))

#define emp_get_block_head(vmr, g) (g)

#define _emp_get_block_head_index(vmr, index, order) \
				({ debug_assert((order) == 0); (index); })
#define emp_get_block_head_index(vmr, index) (index)

/**
 * emp_lock_block - Lock the head of the block
 * @param vmr vmr of the index
 * @parem gpa pointer of gpa subblock address in the block to lock
 * @param index gpa subblock index in the block to lock
 *
 * @return head of the block
 */
static inline struct emp_gpa *
_emp_lock_block(struct emp_vmr *vmr, struct emp_gpa **_gpa, unsigned long index)
{
	struct emp_gpa *gpa = _gpa ? *_gpa : get_gpadesc(vmr, index);
	if (unlikely(!gpa))
		return NULL;
relock:
	// lock head
	while (!____emp_gpa_trylock(gpa))
		cond_resched();

	// check that gpa has not been changed
	if (likely(raw_get_gpadesc(vmr, index) == gpa))
		return gpa;

	gpa = vmr->descs->gpa_dir[index];
	if (_gpa) *_gpa = gpa;

	debug_emp_unlock_block(gpa);

	____emp_gpa_unlock(gpa);
	goto relock;
}

/**
 * emp_trylock_block - Try to lock the head of the block
 * @param vmr vmr of the index
 * @parem gpa pointer of gpa subblock address in the block to lock
 * @param index gpa subblock index in the block to lock
 *
 * @return head of the block
 */
static inline struct emp_gpa *
_emp_trylock_block(struct emp_vmr *vmr, struct emp_gpa **gpa, unsigned long index)
{
	struct emp_gpa *gpa = _gpa ? *_gpa : get_gpadesc(vmr, index);
	if (unlikely(!gpa))
		return NULL;
relock:
	// lock head
	if (!____emp_gpa_trylock(gpa))
		return NULL;

	// check that gpa has not been changed
	if (likely(raw_get_gpadesc(vmr, index) == gpa))
		return gpa;

	gpa = vmr->descs->gpa_dir[index];
	if (_gpa) *_gpa = gpa;

	debug_emp_unlock_block(gpa);

	____emp_gpa_unlock(gpa);
	goto relock;
}

/**
 * emp_unlock_block - Unlock the block
 * @param head head of the block
 */
static inline void _emp_unlock_block(struct emp_gpa *gpa)
{
	debug_emp_unlock_block(gpa);

	____emp_gpa_unlock(gpa);
}

/**
 * __emp_trylock_block - Try to lock the head of the block
 * @param gpa head gpa descriptor of the block
 *
 * @return the gpa if its lock is acquired or NULL
 */
static inline struct emp_gpa *
___emp_trylock_block(struct emp_gpa *gpa)
{
	return ____emp_gpa_trylock(gpa) ? gpa : NULL;
}

/**
 * __emp_lock_block - Lock the head of the block
 * @param gpa head gpa descriptor of the block
 */
static inline void
___emp_lock_block(struct emp_gpa *gpa)
{
	while (!____emp_gpa_trylock(gpa))
		cond_resched();
}

static inline void
___emp_unlock_block(struct emp_gpa *gpa)
{
	debug_assert(____emp_gpa_is_locked(gpa));
	____emp_gpa_unlock(gpa);
}

static inline struct emp_gpa *
_emp_lock_block_local(struct emp_vmr *vmr, struct emp_gpa **gpa)
{
	/* NOTE: when local page is migrated, its gpa index is not chaged */
	return _emp_lock_block(vmr, gpa, get_local_gpa_index(*gpa));
}

static inline struct emp_gpa *
_emp_trylock_block_local(struct emp_vmr *vmr, struct emp_gpa **gpa)
{
	debug_assert((*gpa)->local_page);
	/* NOTE: when local page is migrated, its gpa index is not chaged */
	return _emp_trylock_block(vmr, gpa, get_local_gpa_index(*gpa));
}

static inline struct gpadesc_region *
__get_gpadesc_region_predict(struct emp_vmdesc *desc, unsigned long idx) {
	struct gpadesc_region *region;
	int r;
	for (r = 0; r < desc->num_region; r++) {
		region = &desc->regions[r];
		if (region->start <= idx && idx < region->end)
			return region;
	}
	return NULL; // idx >= desc->gpa_len
}

static inline struct gpadesc_region *
get_gpadesc_region_predict(struct emp_vmdesc *desc, unsigned long idx,
				struct gpadesc_region *predict) {
	if (likely(idx >= predict->start && idx < predict->end))
		return predict;
	return __get_gpadesc_region_predict(desc, idx);
}

static inline struct gpadesc_region *
__get_gpadesc_region(struct emp_vmdesc *desc, unsigned long idx) {
	struct gpadesc_region *region;
	int r;
	for (r = 1; r < desc->num_region; r++) {
		region = &desc->regions[r];
		if (region->start <= idx && idx < region->end)
			return region;
	}
	return NULL; // idx >= desc->gpa_len
}

static inline struct gpadesc_region *
get_gpadesc_region(struct emp_vmdesc *desc, unsigned long idx) {
	struct gpadesc_region *region = &desc->regions[0];
	if (likely(idx >= region->start && idx < region->end))
		return region;
	return __get_gpadesc_region(desc, idx);
}


static inline struct emp_gpa *
__get_next_exist_gpadesc(struct emp_vmr *vmr, unsigned long *indexp) {
	struct emp_gpa *gpa = NULL;
	unsigned long index = *indexp;
	unsigned long gpa_len = vmr->descs->gpa_len;

	// We already check raw_get_gpadesc(vmr, index) == NULL
	do {
		index++;
		if (unlikely(index >= gpa_len))
			break;
		gpa = raw_get_gpadesc(vmr, index);
	} while (!gpa);

	*indexp = index;
	return gpa;
}

static inline struct emp_gpa *
get_next_exist_gpadesc(struct emp_vmr *vmr, unsigned long *index) {
	struct emp_gpa *gpa = raw_get_gpadesc(vmr, *index);
	if (gpa)
		return gpa;
	else
		return __get_next_exist_gpadesc(vmr, index);
}


/* trylock the gpa descriptor that belongs to @lp */
static inline struct emp_gpa *
_emp_trylock_local_page(struct emp_mm *emm, struct local_page *lp)
{
	int vmr_id = lp->vmr_id;
	unsigned long gpa_idx = lp->gpa_index;
	struct emp_vmr *vmr;
	struct emp_gpa *gpa, *head;

	/* Before locking, local_page can be in any state */
	if (unlikely(vmr_id < 0))
		return NULL;
	vmr = emm->vmrs[vmr_id];
	if (unlikely(vmr == NULL || vmr->vmr_closing == true))
		return NULL;
	/* the caller have the list lock. Thus, even if the local page has been
	 * removing by gpas_close(), gpas_close() cannot free the local page
	 * and cannot free vmr since both requires the list lock.
	 */
	gpa = get_exist_gpadesc(vmr, gpa_idx);
	head = _emp_trylock_block(vmr, &gpa, gpa_idx);
	if (head == NULL)
		return NULL;
	if (likely(gpa->local_page == lp && vmr->vmr_closing == false))
		return head;
	else {
		/* local_page may be migrated */
		_emp_unlock_block(head);
		return NULL;
	}
}

static inline void
_emp_unlock_local_page(struct emp_mm *emm, struct local_page *lp)
{
	struct emp_gpa *gpa, *head;
	debug_assert(lp->vmr_id >= 0);
	gpa = get_exist_gpadesc(emm->vmrs[lp->vmr_id], lp->gpa_index);
	debug_assert(gpa->local_page == lp);
	head = emp_get_block_head(gpa);
	debug_assert(____emp_gpa_is_locked(head));
	_emp_unlock_block(head);
}

/* trylock the gpa descriptor that belongs to @w */
static inline struct emp_gpa *
_emp_trylock_work_request(struct emp_mm *emm, struct work_request *w)
{
	struct local_page *lp;
	struct emp_gpa *head;

	/* before locking, anything can happen on @lp. Check anything. */
	lp = w->gpa->local_page;
	if (unlikely(lp->w != w))
		return NULL;

	if (lp->vmr_id < 0) {
		head = emp_get_block_head(w->gpa);
		head = ___emp_trylock_block(head);
		if (head && w->gpa->local_page->w == w)
			return head;
		else
			return NULL;
	}

	head = _emp_trylock_local_page(emm, lp);
	if (head == NULL)
		return NULL;
	/* NOTE: lp == w->gpa->local_page.
	 *       Otherwise, emp_trylock_local_page() returns NULL. */
	debug_assert(head == emp_get_block_head(w->gpa));
	debug_assert(lp->w == w);

	return head;
}

#ifdef CONFIG_EMP_DEBUG
#define __emp_trylock_block(gpa) ({ \
	struct emp_gpa *____ret = ___emp_trylock_block(gpa); \
	if (____ret) debug_progress_gpa_lock(____ret, 0); \
	____ret; })
#define __emp_lock_block(gpa) do { \
	___emp_lock_block(gpa); \
	debug_progress_gpa_lock(gpa, 0); \
} while (0)
#define __emp_unlock_block(gpa) do { \
	debug_progress_gpa_lock(gpa, 0); \
	___emp_unlock_block(gpa); \
} while (0)
#define emp_lock_block(vmr, _gpa, index) ({ \
	struct emp_gpa *____ret = _emp_lock_block(vmr, _gpa, index); \
	if (____ret) debug_progress_gpa_lock(____ret, (index)); \
	____ret; })
#define emp_trylock_block(vmr, _gpa, index) ({ \
	struct emp_gpa *____ret = _emp_trylock_block(vmr, _gpa, index); \
	if (____ret) debug_progress_gpa_lock(____ret, (index)); \
	____ret; })
#define emp_unlock_block(head) do { \
	debug_progress_gpa_lock(head, 0); \
	_emp_unlock_block(head); \
} while (0)
#define emp_lock_block_local(vmr, gpa) ({ \
	struct emp_gpa *____ret; \
	debug_assert((*gpa)->local_page); \
	____ret = _emp_lock_block_local(vmr, gpa); \
	if (____ret) debug_progress_gpa_lock(____ret, gpa); \
	____ret; })
#define emp_trylock_block_local(vmr, gpa) ({ \
	struct emp_gpa *____ret; \
	debug_assert((*gpa)->local_page); \
	____ret = _emp_trylock_block_local(vmr, gpa); \
	if (____ret) debug_progress_gpa_lock(____ret, gpa); \
	____ret; })
#define emp_trylock_local_page(emm, lp) ({ \
	struct emp_gpa *____ret = _emp_trylock_local_page(emm, lp); \
	if (____ret) debug_progress_gpa_lock(____ret, lp); \
	____ret; })
#define emp_unlock_local_page(emm, lp) do { \
	struct emp_gpa *____gpa, *____head; \
	debug_assert((lp)->vmr_id >= 0); \
	____gpa = get_exist_gpadesc((emm)->vmrs[(lp)->vmr_id], (lp)->gpa_index); \
	____head = emp_get_block_head(____gpa); \
	debug_progress_gpa_lock(____head, lp); \
	_emp_unlock_local_page(emm, lp); \
} while (0)
#define emp_trylock_work_request(emm, w) ({ \
	struct emp_gpa *____ret = _emp_trylock_work_request(emm, w); \
	if (____ret) debug_progress_gpa_lock(____ret, w); \
	____ret; })
#else
#define __emp_trylock_block(gpa) ___emp_trylock_block(gpa)
#define __emp_lock_block(gpa) ___emp_lock_block(gpa)
#define __emp_unlock_block(gpa) ___emp_unlock_block(gpa)
#define emp_lock_block(vmr, _gpa, index) _emp_lock_block(vmr, _gpa, index)
#define emp_trylock_block(vmr, _gpa, index) _emp_trylock_block(vmr, _gpa, index)
#define emp_unlock_block(head) _emp_unlock_block(head)
#define emp_lock_block_local(vmr, gpa) _emp_lock_block_local(vmr, gpa)
#define emp_trylock_block_local(vmr, gpa) _emp_trylock_block_local(vmr, gpa)
#define emp_trylock_local_page(emm, lp) _emp_trylock_local_page(emm, lp)
#define emp_unlock_local_page(emm, lp) _emp_unlock_local_page(emm, lp)
#define emp_trylock_work_request(emm, w) _emp_trylock_work_request(emm, w)
#endif

#endif
