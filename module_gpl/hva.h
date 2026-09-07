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

pmd_t *get_populated_pmd(struct mm_struct *, unsigned long, pgtable_t *prealloc);
vm_fault_t emp_page_fault_hva(struct vm_fault *);

#ifdef CONFIG_EMP_USER
/*
 * Give an EMP page the mapping and index of the region that maps it.
 *
 * EMP's pages are file-backed: the mapping is always the EMP device file's
 * address_space, for a private region as much as for a shared one. That is
 * what makes get_futex_key() work on EMP memory for a shared futex (it would
 * return -EFAULT with no mapping), and it keeps the kernel's own view of the
 * page consistent with how EMP maps it, since every pte EMP installs is
 * accounted through the file rmap (folio_add_file_rmap_ptes) and every EMP
 * charge lands on MM_FILEPAGES.
 *
 * The pages are not in the mapping's page cache: nothing looks them up by
 * index, and the aops installed in emp_open() keep the paths that would
 * (folio_mark_dirty) harmless.
 */
static inline void emp_set_page_mapping_and_index(struct vm_area_struct *vma, unsigned long addr, struct page *page)
{
	/* compound page: set mapping/index only on head */
	debug_check_head(page);
	/* ...and @addr is the head's own address, so it is aligned to the size
	 * of the allocation. get_futex_key() derives the key as
	 * folio->index + folio_page_idx(folio, page), adding the faulting
	 * page's offset within the folio itself, so indexing the head by
	 * anything else places the key that far out. Everything which maps a
	 * subblock now hands over the subblock's own address and says
	 * separately which page inside it the mapping starts at. */
	debug_assert((addr & ((PAGE_SIZE << compound_order(page)) - 1)) == 0);
	/* an EMP region is always a mapping of the EMP device file */
	debug_assert(vma->vm_file);
	page->mapping = vma->vm_file->f_mapping;
	page->index   = linear_page_index(vma, addr);
}

/*
 * Clear page->mapping and index before the page leaves EMP.
 */
static inline void emp_clear_page_mapping_and_index(struct page *page)
{
	debug_check_head(page);
	page->mapping = NULL;
	page->index   = 0;
}
#else
#define emp_set_page_mapping_and_index(vma, haddr, page) do {} while (0)
#define emp_clear_page_mapping_and_index(page) do {} while (0)
#endif

#endif /* __HVA_H__ */
