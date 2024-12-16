#ifndef __KVM_EMP_H__
#define __KVM_EMP_H__

#define KVM_HC_EMP_MARK_FREE_PAGES (13)

#ifndef EMP_GUEST
#include <linux/kvm_host.h>

struct kvm_emp_container {
	struct kvm kvm;
	void *emp_mm;
};

struct emp_mod {
	int (*page_fault)(struct kvm_vcpu *, unsigned long, bool, bool *,
			  kvm_pfn_t *, gva_t, int, u32, bool,
			  struct kvm_memory_slot *);
	int (*mark_free_pages)(struct kvm *, gfn_t, long);
	int (*lock_range_pmd)(struct kvm *, unsigned long, void **, void **);
	void (*unlock_range_pmd)(struct kvm *, void *, void *);
};

void register_emp_mod(struct emp_mod *emp);

#define EMP_KVM_LOCK_NOT_MINE	(-EINVAL)
#define EMP_KVM_LOCK_BUSY	(-EBUSY)
#endif /* EMP_GUEST */

#endif /* __KVM_EMP_H__ */
