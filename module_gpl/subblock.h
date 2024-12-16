#ifndef __SUBBLOCK_H__
#define __SUBBLOCK_H__

#include "local_page.h"
#include "debug.h"

/**
 * emp_get_subblock - increase reference count of the pages in a sub-block
 * @param g head of the sub-block
 * @param compund is it compound page?
 */
static inline void __emp_get_subblock(struct emp_gpa *g, bool compound)
{
	struct page *p;

	debug_check_null_pointer(g->local_page);
	p = g->local_page->page;
	if (PageCompound(p)) {
		p = compound_head(p);
		page_ref_add(p, 1);
	} else {
		get_page(p);
	}

}

#define emp_get_subblock(g, compound) do { \
	__emp_get_subblock(g, compound); \
	debug_page_ref_mark((g)->local_page->vmr_id, (g)->local_page, 1); \
} while (0)

#define emp_get_subblock_calibrate(g) do { \
	debug_page_ref_calibrate_beg((g)->local_page, 1); \
	__emp_get_subblock(g, false); \
	debug_page_ref_mark((g)->local_page->vmr_id, (g)->local_page, 0); \
	debug_page_ref_calibrate_end((g)->local_page, 1); \
} while (0)

/**
 * emp_put_subblock - decrease reference count of the pages in a sub-block
 * @param g head of the sub-block
 */
static inline void __emp_put_subblock(struct emp_gpa *g)
{
	struct page *p;

	debug_check_null_pointer(g->local_page);
	p = g->local_page->page;
	put_page(p);
}

#define emp_put_subblock(g) do { \
	__emp_put_subblock(g); \
	debug_page_ref_mark((g)->local_page->vmr_id, (g)->local_page, -1); \
} while (0)

/**
 * emp_lock_subblock - lock the sub-block
 * @param g head of the sub-block
 */
static inline void emp_lock_subblock(struct emp_gpa *g)
{
	struct page *p;

	debug_check_null_pointer(g->local_page);
	p = g->local_page->page;
	lock_page(p);
}

/**
 * emp_unlock_subblock - unlock the sub-block
 * @param g head of the sub-block
 */
static inline void emp_unlock_subblock(struct emp_gpa *g)
{
	struct page *p;

	debug_check_null_pointer(g->local_page);
	p = g->local_page->page;
	unlock_page(p);
}

static inline void emp_get_subblock_except(struct emp_gpa *g, bool compound, int except_idx)
{
	/* except_idx is always -1. nothing to do. */
}

static inline void emp_put_subblock_except(struct emp_gpa *g, int except_idx)
{
	/* nothing to do */
}

#endif
