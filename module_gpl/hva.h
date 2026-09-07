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
 * Give an EMP page what a file-backed page has: the EMP device file's
 * address_space, and the kernel's linear index (its offset in the region plus
 * vma->vm_pgoff). get_futex_key() keys a shared futex on EMP memory by
 * (device inode, page->index), and emp_mmap() gives every region its own
 * slice of the index space so no two EMP pages share a key; see
 * emp_alloc_index_slice() in vm.c. Private futexes, keyed by (mm, address),
 * need none of it. The file mapping also accounts every pte EMP installs
 * through the file rmap and every charge on MM_FILEPAGES.
 *
 * Called at every install and idempotent: the kernel keeps vm_pgoff stable
 * (copies it at fork, adjusts it on a split, keeps it on a move), so every
 * vma that maps a page agrees on its index, which a sleeping waiter's key
 * depends on. The debug build checks it.
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
	debug_assert(!page->mapping
			|| (page->mapping == vma->vm_file->f_mapping
				&& page->index == linear_page_index(vma, addr)));
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
