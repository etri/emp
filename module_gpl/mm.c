#include <linux/version.h>
#include <linux/rmap.h>
#include <linux/writeback.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include "vm.h"
#include "glue.h"
#include "debug.h"
#include "block-flag.h"

/*
 * reference: __do_fault
 */
/*
 * early cow break is not necessary because qemu mmap
 * with VM_SHARED and there's no page_mkwrite function
 */
#define __EARLY_COW_BREAK 0

/**
 * __pte_install - Install page table entries in the host page table
 * @param bvma bvma data structure
 * @param vma vm area where mapping(s) wiil be installed 
 * @param vmf fault address info
 * @param order the size of page
 * @param pmd page middle directory (upper entry of pte)
 * @param orig_pmd original pmd
 * @param partial_map is partially mapped gpa?
 *
 * @retval VM_FAULT_NOPAGE(256): Success
 * @retval 0: Error
 */
int COMPILER_DEBUG
__pte_install(struct emp_mm *bvma,struct vm_area_struct *vma,
		struct vm_fault *vmf, int order, pmd_t *pmd,
		pmd_t orig_pmd, const bool partial_map)
{
	pte_t pte_entry;
	spinlock_t *ptl;
	pte_t *_pte, *pte;
	unsigned long haddr;
	struct page *_page, *page;
	int i, ret = 0, page_compound_len;

	haddr = (unsigned long)vmf->address;
	page_compound_len = (1 << order);


		if (order) {
			haddr = VA_ROUND_DOWN_ORDER(haddr, order + PAGE_SHIFT);
	}
	
	// ptl is spinlock of pmd page
	ptl = pte_lockptr(vma->vm_mm, pmd);
	pte = pte_offset_map(pmd, haddr);

	debug_pte_install(bvma, vmf->page, page_compound_len);

	page = vmf->page;

	spin_lock(ptl);
	for (_pte = pte; _pte < pte + page_compound_len; _pte++) {
		pte_entry = *_pte;
		if (pte_val(pte_entry)) {
			int idx = _pte - pte;
			struct emp_vmr *vmr = (struct emp_vmr *)vma->vm_private_data;
			if (vmr->magic != EMP_VMR_MAGIC_VALUE
					|| vmr->id >= EMP_VMRS_MAX
					|| bvma->vmrs[vmr->id] != vmr)
				vmr = NULL;
			printk(KERN_ERR "%s ERROR: already occupied. "
				"fault_addr: %016lx addr: %016lx "
				"idx: %d pte: %016lx pfn: %lx "
				"page: %016lx pfn: %lx flag: %016lx "
				"page[%d]: %016lx pfn: %lx flag: %016lx "
				"vm_start: %016lx vm_end: %016lx "
				"vm_flag: %016lx vm_base: %016lx\n",
				__func__,
				(unsigned long) vmf->address, haddr,
				idx, pte_val(pte_entry), pte_pfn(pte_entry),
				(unsigned long) page, page_to_pfn(page),
				page->flags,
				idx, (unsigned long) (page + idx),
				page_to_pfn(page + idx), (page + idx)->flags,
				vma->vm_start, vma->vm_end,
				vma->vm_flags,
				vmr ? vmr->descs->vm_base : 0);
			pte_unmap(pte);
			spin_unlock(ptl);
			goto pte_install_failed;
		}
	}

	// now, empty pte is guaranteed
	for (i = 0, _pte = pte, _page = page;
		i < page_compound_len; i++, _pte++, _page++, haddr += PAGE_SIZE) {
		/*
		   following mk_pte could be a problem without compiler optimization
		   with turning off the optimization of gcc (GCC) 4.4.7 20120313 (Red Hat 4.4.7-4)
		   there was a case that variable assignment of a mk_pte did not work correctly
		 */
		flush_icache_page(vma, _page);
		pte_entry = mk_pte(_page, vma->vm_page_prot);
		if (vmf->flags & FAULT_FLAG_WRITE)
			pte_entry = maybe_mkwrite(pte_mkdirty(pte_entry), vma);
		pte_entry = pte_mkold(pte_entry);
		kernel_page_add_file_rmap(_page, vma, false);
		update_mmu_cache(vma, haddr, _pte);
		set_pte_at(vma->vm_mm, haddr, _pte, pte_entry);
	}
	pte_unmap(pte);
	spin_unlock(ptl);

	/* two cases exist.
	 * 	FAULT_FLAG_WRITE: write access
	 * 	!FAULT_FLAG_WRITE: read access
	 */
	if (vmf->flags & FAULT_FLAG_WRITE)
		emp_set_page_dirty(page, page_compound_len);

	ret = VM_FAULT_NOPAGE;

pte_install_failed:
	return ret;

#if __EARLY_COW_BREAK
pte_install_unwritable_page:
	page_cache_release(page);
	return ret;
#endif
}
