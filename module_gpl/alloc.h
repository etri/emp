#ifndef __ALLOC_H__
#define __ALLOC_H__
#include "vm.h"


struct page *get_global_free_page(struct emp_mm *);
bool check_alloc_pages_available(struct emp_mm *);
int wait_pages_available(struct emp_mm *, struct vcpu_var *);
void flush_local_free_pages(struct emp_mm *, struct vcpu_var *);
void alloc_init(struct emp_mm *);
void alloc_exit(struct emp_mm *);

/* push a free page into free page list */
void push_free_page_list(struct emp_mm *, struct page *, struct vcpu_var *);

struct page *_alloc_pages(struct emp_mm *, int, int, struct vcpu_var *);

#endif /* __ALLOC_H__ */
