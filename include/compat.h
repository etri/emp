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
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0))
	// RHEL_RELEASE_VERSION < 9.4 or KERNEL_VERSION < 6.5
#define emp_blkdev_get_by_path(path, mode) blkdev_get_by_path((path), (mode), NULL)
#elif (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 5)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0))
	// 9.4 <= RHEL_RELEASE_VERSION < 9.5 or 6.5 <= KERNEL_VERSION < 6.8
#define emp_blkdev_get_by_path(path, mode) blkdev_get_by_path((path), (mode), NULL, NULL)
#else
	// RHEL_RELEASE_VERSION >= 9.5 or KERNEL_VERSION >= 6.8
/* blkdev_get_no_open() is no longer exported; resolved at runtime via kallsyms.
 * See kernel_blkdev_get_no_open() in module_gpl/glue.c. */
extern struct block_device *kernel_blkdev_get_no_open(dev_t dev);
#define emp_blkdev_get_by_path(path, mode) ({ \
	dev_t ____dev; \
	int ____ret; \
	struct block_device *____bdev; \
	____ret = lookup_bdev((path), &____dev); \
	if (____ret == 0) \
		____bdev = kernel_blkdev_get_no_open(____dev); \
	else \
		____bdev = ERR_PTR(____ret); \
	____bdev; })
#endif

/* After kernel 6.5.0, pte_mkwrite() takes a VMA and is implemented out-of-line
 * (for x86 shadow-stack support) without an EXPORT_SYMBOL. Inline our own
 * variant that uses pte_mkwrite_novma() so the module links. EMP does not use
 * shadow stacks, so skipping that path is safe. */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 5)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0))
#define emp_maybe_mkwrite(pte, vma) maybe_mkwrite((pte), (vma))
#else
#define emp_maybe_mkwrite(pte, vma) ({ \
		pte_t ____pte = (pte); \
		if (likely((vma)->vm_flags & VM_WRITE)) \
			____pte = pte_mkwrite_novma(____pte); \
		____pte; })
#endif

/* After RHEL 9.4 or kernel 6.5.0, we cannot link pte_offset_map(), and
 * pte_unmap() does rcu_read_unlock() (the matching rcu_read_lock() lives in the
 * unlinkable __pte_offset_map()). EMP validates the pmd at the caller and walks
 * PTEs under the PTE lock, so that RCU is unnecessary: use the bare __pte_map()
 * with a no-op counterpart. Do NOT call pte_unmap() here -- it would
 * rcu_read_unlock() with no matching lock. EMP is 64-bit only. */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9,4)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
#define emp_pte_map(pmd, addr) __pte_map((pmd), (addr))
#define emp_pte_unmap(pte) do { (void)(pte); } while (0)
#else
#define emp_pte_map(pmd, addr) pte_offset_map((pmd), (addr))
#define emp_pte_unmap(pte) pte_unmap((pte))
#endif

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
#define EMP_REQ_POLLED (REQ_HIPRI)
#define emp_wr_blk_poll(w) blk_poll((w)->q, (w)->cookie, false)
#else
#define EMP_REQ_POLLED (REQ_POLLED)
#define emp_wr_blk_poll(w) bio_poll((w)->bio, NULL, false)
#endif

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 6)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0))
#define emp_get_user_pages(start, nr_pages, gup_flags, pages) get_user_pages(start, nr_pages, gup_flags, pages, NULL)
#else
#define emp_get_user_pages(start, nr_pages, gup_flags, pages) get_user_pages(start, nr_pages, gup_flags, pages)
#endif

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 5)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0))
#define EMP_VMA_ITERATOR(name, __mm, __addr)	\
		struct vm_area_struct *name = (__mm)->mmap
#define emp_for_each_vma(vmi, vma) for ((vma) = (vmi); (vma); (vma) = (vma)->vm_next)
#else
#define EMP_VMA_ITERATOR(name, __mm, __addr) VMA_ITERATOR(name, __mm, __addr)
#define emp_for_each_vma(vmi, vma) for_each_vma(vmi, vma)
#endif

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 5)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0))
#define emp_mmu_notifier_range_to_vma(range) ((range)->vma)
#define vm_flags_set(vma, __flags) do { (vma)->vm_flags |= (__flags); } while (0)
#else
#define emp_mmu_notifier_range_to_vma(range) vma_lookup((range)->mm, (range)->start)
#endif

#endif /* __COMPAT_H__ */
