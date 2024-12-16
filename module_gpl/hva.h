#ifndef __VMA_H__
#define __VMA_H__

#include <linux/version.h>
#include <linux/mm_types.h>
#include "vm.h"

int emp_page_fault_hptes_map(struct emp_mm *, struct emp_vmr *,
			struct emp_gpa *, struct emp_gpa *, unsigned long,
			struct emp_gpa *, struct emp_gpa *,
			bool, struct vm_fault *, bool);

pmd_t *get_pmd(struct mm_struct *, unsigned long, pmd_t **);
vm_fault_t emp_page_fault_hva(struct vm_fault *);

#endif /* __VMA_H__ */
