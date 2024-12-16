#ifndef __MM_H__
#define __MM_H__

int __pte_install(struct emp_mm *, struct vm_area_struct *, struct vm_fault *,
		int , pmd_t *, pmd_t, bool);

#define pte_install(bvma, vma, fault, sb_order, pmd, orig_pmd, head) \
		__pte_install(bvma, vma, fault, sb_order, pmd, orig_pmd, false)

#endif /* __MM_H__ */
