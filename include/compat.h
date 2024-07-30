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
#elif (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))
	// RHEL_RELEASE_VERSION > 9.0 or KERNEL_VERSION >= 5.18.0
#define load_acquire_kvm_tlbs_dirty_or_0(kvm) (0)
#define get_kvm_tlbs_dirty_or_0(kvm) (0)
#define cmpxchg_kvm_tlbs_dirty(kvm, old, new) do {} while (0)
// type of @bkt is size_t
#define emp_kvm_for_each_memslot(memslot, bkt, slots) kvm_for_each_memslot(memslot, bkt, slots)
#define kvm_get_any_vcpu(kvm) (kvm_get_vcpu(kvm, 0))
#else
	// (RHEL_RELEASE_VERSION =< 9.0 and RHEL_RELEASE >= "425") or KERNEL_VERSION >= 5.18.0
#define load_acquire_kvm_tlbs_dirty_or_0(kvm) (0)
#define get_kvm_tlbs_dirty_or_0(kvm) (0)
#define cmpxchg_kvm_tlbs_dirty(kvm, old, new) do {} while (0)
// type of @bkt is size_t
#define emp_kvm_for_each_memslot(memslot, bkt, slots) kvm_for_each_memslot(memslot, bkt, slots)
#define kvm_get_any_vcpu(kvm) (kvm_get_vcpu(kvm, 0))
#endif

/* After RHEL 9.4 or kernel 6.8.0, KVM_ADDRESS_SPACE_NUM is removed. */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0))
	// RHEL_RELEASE_VERSION >= 9.4 or KERNEL_VERSION >= 6.8.0
#define kvm_nr_memslot(kvm) kvm_arch_nr_memslot_as_ids(kvm)
#else
#define kvm_nr_memslot(kvm) (KVM_ADDRESS_SPACE_NUM)
#endif

/* After RHEL 9.4 or kernel 6.2.0, mm->rss_stat is a percpu_counter.
 * After RHEL 9.0 or kernel 5.18.0, we use atomic_long_add() only. */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0))
#define emp_add_mm_counter(mm, member, value) do { percpu_counter_add(&mm->rss_stat[member], value); } while (0)
#elif (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))
#define emp_add_mm_counter(mm, member, value) do { atomic_long_add(value, &mm->rss_stat.count[member]); } while (0)
#else
	// RHEL_RELEASE_VERSION < 9.0 or KERNEL_VERSION < 5.18.0
#define emp_add_mm_counter(mm, member, value) do { add_mm_counter(mm, member, value); } while (0)
#endif

/* After RHEL 9.4 or kernel 6.4.0, class_create() does not take @owner. */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))
	// RHEL_RELEASE_VERSION >= 9.4 or KERNEL_VERSION >= 6.4.0
#define emp_class_create(owner, name) class_create(name)
#else
#define emp_class_create(owner, name) class_create(owner, name)
#endif

/* After kernel 6.8.0, blkdev_get_by_path() is removed. Use lookup_bdev() and blkdev_get_no_open() instead.
 * After RHEL 9.4 or kernel 6.5.0, blkdev_get_by_path() takes 4 arguments. The fourth argument is struct blk_holder_ops. */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9,4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0))
	// RHEL_RELEASE_VERSION < 9.4 or KERNEL_VERSION < 6.5
#define emp_blkdev_get_by_path(path, mode) blkdev_get_by_path((path), (mode), NULL)
#elif (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9,4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0))
	// RHEL_RELEASE_VERSION >= 9.4 or 6.5 <= KERNEL_VERSION < 6.8
#define emp_blkdev_get_by_path(path, mode) blkdev_get_by_path((path), (mode), NULL, NULL)
#else
	// KERNEL_VERSION >= 6.8
#define emp_blkdev_get_by_path(path, mode) ({ \
	dev_t ____dev; \
	int ____ret; \
	struct block_device *____bdev; \
	____ret = lookup_bdev((path), &____dev); \
	if (____ret == 0) \
		____bdev = blkdev_get_no_open(____dev); \
	else \
		____bdev = ERR_PTR(____ret); \
	____bdev; })
#endif

/* After RHEL 9.4 or kernel 6.5.0, we cannot link pte_offset_map(). */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9,4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
	// RHEL_RELEASE_VERSION >= 9.4 or 6.5 <= KERNEL_VERSION < 6.8
#define emp_pte_offset_map(pmd, addr) ({ \
		pmd_t ____pmdval = pmdp_get_lockless(pmd); \
		pte_t *____ret; \
		if (unlikely(pmd_none(____pmdval) \
				|| pmd_trans_huge(____pmdval) \
				|| pmd_devmap(____pmdval) \
				|| pmd_bad(____pmdval))) \
			____ret = NULL; \
		else \
			____ret = __pte_map(&____pmdval, addr); \
		____ret; })
#else
#define emp_pte_offset_map(pmd, addr) ({ pte_offset_map(pmd, addr); })
#endif

#endif /* __COMPAT_H__ */
