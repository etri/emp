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

/* mmap_lock was renamed from mmap_sem in upstream Linux 5.8.
 * It also provides the mmap_read_lock() helpers.
 * We support RHEL 8+ versions, and all of them provide mmap_read_lock().
 */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0))
#define mmap_read_lock(mm) down_read(&(mm)->mmap_sem)
#define mmap_read_unlock(mm) up_read(&(mm)->mmap_sem)
#define mmap_write_lock(mm) down_write(&(mm)->mmap_sem)
#define mmap_write_unlock(mm) up_write(&(mm)->mmap_sem)
#endif

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


/* Upstream converted these page flags to FOLIO_FLAG(), which emits only
 * folio_{test,set,clear}_<flag>() and drops the Page<Flag>()/SetPage<Flag>()/
 * ClearPage<Flag>() accessors. It did NOT do so in one release:
 *
 *	referenced	6.10	was PAGEFLAG(Referenced, referenced, PF_HEAD)
 *	unevictable	6.12	was PAGEFLAG(Unevictable, unevictable, PF_HEAD)
 *	mlocked		6.12	was PAGEFLAG(Mlocked, mlocked, PF_NO_TAIL)
 *
 * so referenced needs its own, earlier gate. A single 6.12 gate would fail to
 * build on 6.10 and 6.11, where PageReferenced() is already gone. Boundaries
 * read from include/linux/page-flags.h at v6.9/v6.10/v6.11/v6.12; every other
 * flag EMP uses (dirty, LRU, reserved, private, locked, hwpoison) is still
 * PAGEFLAG() in 6.12 and needs no shim. RHEL 10 is 6.12-based, so it is past
 * both boundaries.
 *
 * The shims are exact. In the folio accessors every old policy -- PF_HEAD,
 * PF_NO_TAIL, PF_ANY -- resolves to FOLIO_HEAD_PAGE == 0, i.e. &folio->flags,
 * the head page's flag word, which is where these flags always lived; and
 * page_folio() resolves any page of a compound allocation to that same head.
 * Every EMP call site already passes a subblock head, so the redirection never
 * fires, and for a 4 KiB subblock (order 0) the page is its own folio and the
 * shim is the identical bit operation on the identical word.
 *
 * One delta: Mlocked was PF_NO_TAIL, which carried a
 * VM_BUG_ON_PGFLAGS(PageTail(page)) in a debug kernel. The folio form has no
 * such check; EMP's own debug_check_head() in block-flag.h guards that path.
 *
 * No <linux/mm.h> include is added here on purpose: compat.h is pulled in very
 * early, and these are macros, expanded only at their use sites, all of which
 * already have the page/folio API in scope through vm.h.
 */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(10, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0))
	// RHEL_RELEASE_VERSION >= 10.0 or KERNEL_VERSION >= 6.10.0
#define EMP_HAVE_FOLIO_ONLY_REFERENCED 1
#else
#define EMP_HAVE_FOLIO_ONLY_REFERENCED 0
#endif

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(10, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
	// RHEL_RELEASE_VERSION >= 10.0 or KERNEL_VERSION >= 6.12.0
#define EMP_HAVE_FOLIO_ONLY_UNEVICTABLE_MLOCKED 1
#else
#define EMP_HAVE_FOLIO_ONLY_UNEVICTABLE_MLOCKED 0
#endif

#if EMP_HAVE_FOLIO_ONLY_REFERENCED
#define PageReferenced(p)       folio_test_referenced(page_folio(p))
#define SetPageReferenced(p)    folio_set_referenced(page_folio(p))
#define ClearPageReferenced(p)  folio_clear_referenced(page_folio(p))
#endif /* EMP_HAVE_FOLIO_ONLY_REFERENCED */

#if EMP_HAVE_FOLIO_ONLY_UNEVICTABLE_MLOCKED
#define PageUnevictable(p)      folio_test_unevictable(page_folio(p))
#define SetPageUnevictable(p)   folio_set_unevictable(page_folio(p))
#define ClearPageUnevictable(p) folio_clear_unevictable(page_folio(p))

#define PageMlocked(p)          folio_test_mlocked(page_folio(p))
#define SetPageMlocked(p)       folio_set_mlocked(page_folio(p))
#define ClearPageMlocked(p)     folio_clear_mlocked(page_folio(p))
#endif /* EMP_HAVE_FOLIO_ONLY_UNEVICTABLE_MLOCKED */

/* QUEUE_FLAG_POLL moved into queue_limits.features as BLK_FEAT_POLL in 6.11
 * (v6.10 has the flag, v6.11 the feature). */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(10, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0))
	// RHEL_RELEASE_VERSION >= 10.0 or KERNEL_VERSION >= 6.11.0
#define emp_blk_queue_poll(q) (((q)->limits.features & BLK_FEAT_POLL) != 0)
#else
#define emp_blk_queue_poll(q) test_bit(QUEUE_FLAG_POLL, &(q)->queue_flags)
#endif

#endif /* __COMPAT_H__ */
