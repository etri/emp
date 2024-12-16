#ifndef __SUBBLOCK_H__
#define __SUBBLOCK_H__

#include "local_page.h"
#include "debug.h"

#ifdef CONFIG_EMP_BLOCK
/**
 * offset_in_subblock - get the offset of the page from the head of sub-block
 * @param sbh head of the sub-block
 * @param gpa gpa of the page
 *
 * @return offset
 */
static inline unsigned int offset_in_subblock(struct emp_gpa *sbh, unsigned long gpa)
{
	unsigned long mask;

	mask = ((1 << (gpa_subblock_order(sbh) + PAGE_SHIFT)) - 1);
	return gpa & mask;
}

/**
 * emp_get_subblock - increase reference count of the pages in a sub-block
 * @param g head of the sub-block
 * @param compund is it compound page?
 */
static inline void __emp_get_subblock(struct emp_gpa *g, bool compound)
{
	struct page *p, *e;

	debug_check_null_pointer(g->local_page);
	p = g->local_page->page;
	if (PageCompound(p)) {
		p = compound_head(p);
		page_ref_add(p, compound? gpa_subblock_size(g): 1);
		return;
	} else {
		e = p + gpa_subblock_size(g);
	}

	for (; p < e; p++)
		get_page(p);
}

#define emp_get_subblock(g, compound) do { \
	__emp_get_subblock(g, compound); \
	debug_page_ref_mark((g)->local_page->vmr_id, (g)->local_page, \
		PageCompound((g)->local_page->page) && (compound)  \
					? gpa_subblock_size(g) : 1); \
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
	struct page *p, *e;

	debug_check_null_pointer(g->local_page);
	p = g->local_page->page;
	e = p + (PageCompound(p)? 1: gpa_subblock_size(g));

	for (; p < e; p++)
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
	struct page *p, *e;

	debug_check_null_pointer(g->local_page);
	p = g->local_page->page;
	e = p + (PageCompound(p)? 1: gpa_subblock_size(g));

	for (; p < e; p++)
		lock_page(p);
}

/**
 * emp_unlock_subblock - unlock the sub-block
 * @param g head of the sub-block
 */
static inline void emp_unlock_subblock(struct emp_gpa *g)
{
	struct page *p, *e;

	debug_check_null_pointer(g->local_page);
	p = g->local_page->page;
	e = p + (PageCompound(p)? 1: gpa_subblock_size(g));

	for (; p < e; p++)
		unlock_page(p);
}

/**
 * emp_put_subblock_except - Decrease reference count of the pages in a sub-block
 * 			     except a specific page
 * @param g head of the sub-block
 * @param except_idx except page index (-1: No exception)
 *
 * TODO: Can be merged with emp_put_subblock
 */
static inline void __emp_put_subblock_except(struct emp_gpa *g, int except_idx)
{
	struct page *p, *e, *except = NULL;

	debug_check_null_pointer(g->local_page);
	p = g->local_page->page;
	if (PageCompound(p)) {
		/* EPT: do not put when PageHead(p + except_idx)
		 * HPT: do not put */
		if (PageHead(p + except_idx))
			return;
		p = compound_head(p);
		put_page(p);
		return;
	} else {
		e = p + gpa_subblock_size(g);
	}

	if (except_idx == -1)
		return;

	except = p + except_idx;
	for (; p < e; p++) {
		if (p == except)
			continue;
		put_page(p);
	}
}

#define emp_put_subblock_except(g, except_idx) do { \
	__emp_put_subblock_except(g, except_idx); \
	debug_page_ref_mark((g)->local_page->vmr_id, (g)->local_page,  \
		((PageCompound((g)->local_page->page) && PageHead((g)->local_page->page + (except_idx))) \
		|| (!PageCompound((g)->local_page->page)  && ((except_idx) == -1 || (except_idx) == 0))) \
		 ? 0 : - 1); \
} while (0)

#else /* !CONFIG_EMP_BLOCK */
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
#endif /* !CONFIG_EMP_BLOCK */

#endif
