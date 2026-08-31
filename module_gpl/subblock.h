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
 */
static inline void __emp_get_subblock(struct emp_gpa *g)
{
	debug_check_null_pointer(g->local_page);
	debug_check_head(g->local_page->page);
	get_page(g->local_page->page);
}

#define emp_get_subblock(g) do { \
	__emp_get_subblock(g); \
	debug_page_ref_mark(emp_vmr_dbgid(emp_lp_owner((g)->local_page)), (g)->local_page, 1); \
} while (0)

#define emp_get_subblock_calibrate(g) do { \
	__emp_get_subblock(g); \
	debug_page_ref_calibrate_beg((g)->local_page, 1); \
	debug_page_ref_mark(emp_vmr_dbgid(emp_lp_owner((g)->local_page)), (g)->local_page, 1); \
} while (0)

/**
 * emp_put_subblock - decrease reference count of the pages in a sub-block
 * @param g head of the sub-block
 */
static inline void __emp_put_subblock(struct emp_gpa *g)
{
	debug_check_null_pointer(g->local_page);
	debug_check_head(g->local_page->page);
	put_page(g->local_page->page);
}

#define emp_put_subblock(g) do { \
	__emp_put_subblock(g); \
	debug_page_ref_mark(emp_vmr_dbgid(emp_lp_owner((g)->local_page)), (g)->local_page, -1); \
} while (0)

/**
 * emp_lock_subblock - lock the sub-block
 * @param g head of the sub-block
 */
static inline void emp_lock_subblock(struct emp_gpa *g)
{
	debug_check_null_pointer(g->local_page);
	debug_check_head(g->local_page->page);
	lock_page(g->local_page->page);
}

/**
 * emp_unlock_subblock - unlock the sub-block
 * @param g head of the sub-block
 */
static inline void emp_unlock_subblock(struct emp_gpa *g)
{
	debug_check_null_pointer(g->local_page);
	debug_check_head(g->local_page->page);
	unlock_page(g->local_page->page);
}

#else /* !CONFIG_EMP_BLOCK */
/**
 * emp_get_subblock - increase reference count of the pages in a sub-block
 * @param g head of the sub-block
 */
static inline void __emp_get_subblock(struct emp_gpa *g)
{
	debug_check_null_pointer(g->local_page);
	get_page(g->local_page->page);
}

#define emp_get_subblock(g) do { \
	__emp_get_subblock(g); \
	debug_page_ref_mark(emp_vmr_dbgid(emp_lp_owner((g)->local_page)), (g)->local_page, 1); \
} while (0)

#define emp_get_subblock_calibrate(g) do { \
	debug_page_ref_calibrate_beg((g)->local_page, 1); \
	__emp_get_subblock(g); \
	debug_page_ref_mark(emp_vmr_dbgid(emp_lp_owner((g)->local_page)), (g)->local_page, 0); \
	debug_page_ref_calibrate_end((g)->local_page, 1); \
} while (0)

/**
 * emp_put_subblock - decrease reference count of the pages in a sub-block
 * @param g head of the sub-block
 */
static inline void __emp_put_subblock(struct emp_gpa *g)
{
	debug_check_null_pointer(g->local_page);
	put_page(g->local_page->page);
}

#define emp_put_subblock(g) do { \
	__emp_put_subblock(g); \
	debug_page_ref_mark(emp_vmr_dbgid(emp_lp_owner((g)->local_page)), (g)->local_page, -1); \
} while (0)

/**
 * emp_lock_subblock - lock the sub-block
 * @param g head of the sub-block
 */
static inline void emp_lock_subblock(struct emp_gpa *g)
{
	debug_check_null_pointer(g->local_page);
	lock_page(g->local_page->page);
}

/**
 * emp_unlock_subblock - unlock the sub-block
 * @param g head of the sub-block
 */
static inline void emp_unlock_subblock(struct emp_gpa *g)
{
	debug_check_null_pointer(g->local_page);
	unlock_page(g->local_page->page);
}
#endif /* !CONFIG_EMP_BLOCK */

#endif
