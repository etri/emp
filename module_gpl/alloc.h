#ifndef __ALLOC_H__
#define __ALLOC_H__
#include "vm.h"
#include "debug_assert.h"

struct page *get_global_free_page(struct emp_mm *);
bool check_alloc_pages_available(struct emp_mm *);
int wait_pages_available(struct emp_mm *, struct vcpu_var *);
void flush_local_free_pages(struct emp_mm *, struct vcpu_var *);
void alloc_init(struct emp_mm *);
void alloc_exit(struct emp_mm *);

/* push a free page into free page list */
void push_free_page_list(struct emp_mm *, struct page *, struct vcpu_var *);

struct page *_alloc_pages(struct emp_mm *, int, int, struct vcpu_var *);

static inline void __emp_free_pages(struct page *page) {
	debug_BUG_ON(PageCompound(page) && !PageHead(page));
	debug_BUG_ON(page_ref_count(page) != 1);
	put_page(page); // do not use emp_put_page(). This is defined as "free" functions on debug.h
}

#endif /* __ALLOC_H__ */
