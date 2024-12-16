#ifndef __BLOCK_FLAG_H__
#define __BLOCK_FLAG_H__

#include "vm.h"
#include "block.h"
#include "paging.h"

static inline void _emp_lock_page(struct page *page, int order)
{
	struct page *p, *e;

	debug_check_head(page);
	
	if (PageCompound(page)) {
		lock_page(page);
		return;
	}
	e = page + (1 << order);
	for (p = page; p < e; p++)
		lock_page(p);
}

static inline void _emp_unlock_page(struct page *page, int order)
{
	struct page *p, *e;

	debug_check_head(page);
	
	if (PageCompound(page)) {
		unlock_page(page);
		return;
	}
	e = page + (1 << order);
	for (p = page; p < e; p++)
		unlock_page(p);
}

static inline bool emp_page_mapped(struct page *page, int size)
{
	struct page *p, *e;

	if (PageCompound(page))
		return page_mapped(page);

	e = page + size;
	for (p = page; p < e; p++) {
		if (page_mapped(p))
			return true;
	}
	return false;
}

/* Return true if any of pages is dirty. */
static inline bool emp_set_page_clean(struct page *page, int size)
{
	bool ret = false;
	struct page *p, *e = page + size;
	for (p = page; p < e; p++) {
		if (PageDirty(p)) {
			ClearPageDirty(p);
			ret = true;
		}
	}

	return ret;
}

static inline void emp_set_page_dirty(struct page *page, int size)
{
	struct page *p, *e = page + size;
	for (p = page; p < e; p++) {
		if (!PageDirty(p))
			SetPageDirty(p);
	}
}

static inline bool emp_any_page_dirty(struct page *page, int size)
{
	struct page *p, *e = page + size;
	for (p = page; p < e; p++) {
		if (PageDirty(p))
			return true;
	}
	return false;
}

static inline void emp_clear_pg_mlocked(struct page *page, int order)
{
	struct page *p, *e;
	
	debug_check_head(page);

	if (PageCompound(page)) {
		clear_pg_mlocked(page);
		return;
	}
	
	e = page + (1 << order);
	for (p = page; p < e; p++) {
		clear_pg_mlocked(p);
	}
}

static inline void emp_set_pg_mlocked(struct page *page, int order)
{
	struct page *p, *e;
	
	debug_check_head(page);

	if (PageCompound(page)) {
		set_pg_mlocked(page);
		return;
	}
	e = page + (1 << order);
	for (p = page; p < e; p++) {
		set_pg_mlocked(p);
	}
}

static inline int emp_page_count_max(struct page *page, int size)
{
	struct page *p, *e = page + size;
	int max;

	if (PageCompound(page))
		return page_count(page);

	for (p = (page + 1), max = page_count(page); p < e; p++) {
		if (page_count(p) > max)
			max = page_count(p);
	}
	return max;
}

static inline int emp_page_count_min(struct page *page, int size)
{
	struct page *p, *e = page + size;
	int min;
	
	if (PageCompound(page))
		return page_count(page);

	for (p = (page + 1), min = page_count(page); p < e; p++) {
		if (page_count(p) < min)
			min = page_count(p);
	}
	return min;
}
#endif
