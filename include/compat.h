#ifndef __COMPAT_H__
#define __COMPAT_H__
#include <linux/version.h>
#include <linux/types.h>

// For Ubuntu, RHEL_RELEASE_* are not defined.
#ifndef RHEL_RELEASE_CODE
#define RHEL_RELEASE_CODE (-1)
#endif
#ifndef RHEL_RELEASE_VERSION
#define RHEL_RELEASE_VERSION(a, b) (0)
#endif

/********** Compatibility Code Example ****************************************
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE <= RHEL_RELEASE_VERSION(8, 4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE <= KERNEL_VERSION(5, 6, 19))
	// RHEL_RELEASE <= "240" or KERNEL_VERSION <= 5.6.19
#else
	// RHEL_RELEASE > "240" or KERNEL_VERSION > 5.6.19
#endif
*******************************************************************************/


/* Between 8.3 <= RHEL_RELEASE_CODE < 8.7, the followings are chaned.
 * 1. kvm->tlbs->dirty is removed
 * 2. kvm_for_each_memslot() receives 3 arguments
 * kvm->vcpus[] is changed to an xa_array
 */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8, 7)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0))
	// RHEL_RELEASE < "425" or KERNEL_VERSION < 5.18.0
#include <asm/barrier.h>
#define load_acquire_kvm_tlbs_dirty_or_0(kvm) (smp_load_acquire(&kvm->tlbs_dirty))
#define get_kvm_tlbs_dirty_or_0(kvm) (kvm->tlbs_dirty)
#define cmpxchg_kvm_tlbs_dirty(kvm, old, new) (cmpxchg(&kvm->tlbs_dirty, old, new))
#define emp_kvm_for_each_memslot(memslot, bkt, slots) kvm_for_each_memslot(memslot, slots)
#define kvm_get_any_vcpu(kvm) ((kvm)->vcpus[0])
#define emp_add_mm_counter(mm, member, value) (add_mm_counter(mm, member, value))
#elif (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))
	// RHEL_RELEASE_VERSION > 9.0 or KERNEL_VERSION >= 5.18.0
#define load_acquire_kvm_tlbs_dirty_or_0(kvm) (0)
#define get_kvm_tlbs_dirty_or_0(kvm) (0)
#define cmpxchg_kvm_tlbs_dirty(kvm, old, new) do {} while (0)
// type of @bkt is size_t
#define emp_kvm_for_each_memslot(memslot, bkt, slots) kvm_for_each_memslot(memslot, bkt, slots)
#define kvm_get_any_vcpu(kvm) (kvm_get_vcpu(kvm, 0))
#define emp_add_mm_counter(mm, member, value) (atomic_long_add(value, &mm->rss_stat.count[member]))
#else
	// (RHEL_RELEASE_VERSION =< 9.0 and RHEL_RELEASE >= "425") or KERNEL_VERSION >= 5.18.0
#define load_acquire_kvm_tlbs_dirty_or_0(kvm) (0)
#define get_kvm_tlbs_dirty_or_0(kvm) (0)
#define cmpxchg_kvm_tlbs_dirty(kvm, old, new) do {} while (0)
// type of @bkt is size_t
#define emp_kvm_for_each_memslot(memslot, bkt, slots) kvm_for_each_memslot(memslot, bkt, slots)
#define kvm_get_any_vcpu(kvm) (kvm_get_vcpu(kvm, 0))
#define emp_add_mm_counter(mm, member, value) (add_mm_counter(mm, member, value))
#endif

#endif /* __COMPAT_H__ */
