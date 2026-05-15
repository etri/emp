#ifndef __HVA_H__
#define __HVA_H__

#include <linux/version.h>
#include <linux/mm_types.h>
#include "vm.h"

int emp_page_fault_hptes_map(struct emp_mm *, struct emp_vmr *,
			struct emp_gpa *, struct emp_gpa *, unsigned long,
			bool, struct vm_fault *, bool);
void sync_hpt_map_in_block(struct emp_mm *, struct emp_gpa *, const bool);
void clear_gpa_prefetched_hpt(struct emp_mm *emm, struct emp_vmr *vmr,
				struct emp_gpa *head, unsigned long head_idx);

pmd_t *get_pmd(struct mm_struct *, unsigned long, pmd_t **);
vm_fault_t emp_page_fault_hva(struct vm_fault *);

#ifdef CONFIG_EMP_USER
/*
 * Set page->mapping and page->index so that get_futex_key() does
 * not return -EFAULT when a shared futex (without FUTEX_PRIVATE_FLAG)
 * is used on emp-managed memory.
 */
static inline void emp_set_page_mapping_and_index(struct vm_area_struct *vma, unsigned long addr, struct page *page, int len)
{
	if (PageCompound(page)) {
		/* compound page: set mapping/index only on head */
		debug_assert(PageHead(page));
		if (vma->vm_flags & VM_SHARED) {
			page->mapping = vma->vm_file->f_mapping;
			page->index   = linear_page_index(vma, addr);
		} else {
			page->mapping = (struct address_space *)
				((unsigned long)vma->anon_vma | PAGE_MAPPING_ANON);
			page->index   = linear_page_index(vma, addr);
		}
	} else {
		/* non-compound page: set mapping/index on every page */
		int i;
		if (vma->vm_flags & VM_SHARED) {
			for (i = 0; i < len; i++, page++, addr += PAGE_SIZE) {
				page->mapping = vma->vm_file->f_mapping;
				page->index   = linear_page_index(vma, addr);
			}
		} else {
			for (i = 0; i < len; i++, page++) {
				page->mapping = (struct address_space *)
					((unsigned long)vma->anon_vma | PAGE_MAPPING_ANON);
				page->index   = linear_page_index(vma, addr);
			}
		}
	}
}

/*
 * Clear page->mapping and index which were set to support shared futexes.
 */
static inline void emp_clear_page_mapping_and_index(struct page *page, int order)
{
	if (PageCompound(page)) {
		/* compound page: set mapping/index only on head */
		debug_assert(PageHead(page));
		page->mapping = NULL;
		page->index   = 0;
	} else {
		/* non-compound page: set mapping/index on every page */
		int i, len = 1 << order;
		for (i = 0; i < len; i++, page++) {
			page->mapping = NULL;
			page->index   = 0;
		}
	}
}
#else
#define emp_set_page_mapping_and_index(vma, haddr, page, len) do {} while (0)
#define emp_clear_page_mapping_and_index(page, order) do {} while (0)
#endif

#endif /* __HVA_H__ */
