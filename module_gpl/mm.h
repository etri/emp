#ifndef __MM_H__
#define __MM_H__

int __pte_install(struct emp_mm *, struct vm_area_struct *, struct vm_fault *,
		int , pmd_t *, pmd_t, bool);

#ifdef CONFIG_EMP_USER
#define pte_install(bvma, vma, fault, sb_order, pmd, orig_pmd, head) \
		__pte_install(bvma, vma, fault, sb_order, pmd, orig_pmd, \
			is_gpa_flags_set((head), GPA_PARTIAL_MAP_MASK))
#else
#define pte_install(bvma, vma, fault, sb_order, pmd, orig_pmd, head) \
		__pte_install(bvma, vma, fault, sb_order, pmd, orig_pmd, false)
#endif

#endif /* __MM_H__ */
