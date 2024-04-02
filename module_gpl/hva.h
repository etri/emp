#ifndef __VMA_H__
#define __VMA_H__

#include <linux/version.h>
#include <linux/mm_types.h>
#include "vm.h"

int emp_page_fault_hptes_map(struct emp_mm *, struct emp_vmr *,
			struct emp_gpa *, struct emp_gpa *, unsigned long,
			bool, struct vm_fault *, bool);
int emp_install_hptes(struct emp_mm *bvma, struct emp_vmr *vmr,
			struct emp_gpa *head, struct emp_gpa *demand,
			pmd_t *pmd, bool prefetch_hit, bool is_write);

pmd_t *get_pmd(struct mm_struct *, unsigned long, pmd_t **);
vm_fault_t emp_page_fault_hva(struct vm_fault *);

#endif /* __VMA_H__ */
