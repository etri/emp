#ifndef __BLOCK_FLAG_H__
#define __BLOCK_FLAG_H__

#include "vm.h"
#include "block.h"
#include "paging.h"

static inline void _emp_lock_page(struct page *page)
{
	debug_check_head(page);
	lock_page(page);
}

static inline void _emp_unlock_page(struct page *page)
{
	debug_check_head(page);
	unlock_page(page);
}

static inline bool emp_page_mapped(struct page *page)
{
	debug_check_head(page);
	return page_mapped(page);
}

/* Return true if any of pages is dirty. */
static inline bool emp_set_page_clean(struct page *page)
{
	bool ret;
	debug_check_head(page);
	ret = PageDirty(page);
	ClearPageDirty(page);
	return ret;
}

static inline void emp_set_page_dirty(struct page *page)
{
	debug_check_head(page);
	SetPageDirty(page);
}

static inline bool emp_any_page_dirty(struct page *page)
{
	debug_check_head(page);
	return PageDirty(page);
}

static inline void emp_clear_pg_mlocked(struct page *page)
{
	debug_check_head(page);
	clear_pg_mlocked(page);
}

static inline void emp_set_pg_mlocked(struct page *page)
{
	debug_check_head(page);
	set_pg_mlocked(page);
}

static inline int emp_page_count(struct page *page)
{
	debug_check_head(page);
	return page_count(page);
}
#endif
