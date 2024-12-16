#ifndef __PAGE_MGMT_H__
#define __PAGE_MGMT_H__

#ifdef CONFIG_EMP_VM
#include <linux/kvm_host.h>
#include "kvm_mmu.h"

static inline int is_shadow_present_pte(u64 pte)
{
	return (pte != 0) && !kvm_emp_is_mmio_spte(pte);
}

int emp_page_fault_gpa(struct kvm_vcpu *, const unsigned long, bool, bool *,
		u64 *, gva_t, int, u32, bool, struct kvm_memory_slot *);
u64 *get_spte(struct kvm_vcpu *vcpu, int level, u64 offset);
#endif

int handle_active_fault(struct emp_vmr *, struct emp_gpa *, struct emp_gpa *,
			struct vcpu_var *, struct vm_fault *, int *);
void handle_inactive_fault(struct emp_vmr *, struct emp_gpa **,
			   struct emp_gpa *, struct vcpu_var *);
int handle_writeback_fault(struct emp_vmr *, struct emp_gpa **,
			    struct emp_gpa *, struct vcpu_var *);
int handle_remote_fault(struct emp_vmr *, struct emp_gpa **, unsigned long,
			struct emp_gpa *, pgoff_t, struct vcpu_var *, bool);
int handle_local_fault(struct emp_vmr *, struct emp_gpa **, struct emp_gpa *,
		       struct vcpu_var *, struct vm_fault *, int *);
#ifdef CONFIG_EMP_VM
u64 emp_page_fault_sptes_map(struct kvm_vcpu *, struct vcpu_var *,
			     struct emp_gpa *, u64, struct kvm_memory_slot *);
int emp_mark_free_pages(struct kvm *, gfn_t, long);

int emp_lock_range_pmd(struct kvm *, unsigned long, void **, void **);
void emp_unlock_range_pmd(struct kvm *, void *, void *);
#endif
#endif /* __PAGE_MGMT_H__ */
