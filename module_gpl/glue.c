#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/memcontrol.h>
#include <linux/version.h>
#include <linux/kvm_host.h>
#include <asm/pgtable.h>
#include <linux/nvme.h>
#include <linux/blkdev.h>
#include <linux/kallsyms.h>
#include <compat.h>
#include "glue.h"
#include "debug.h"

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0))
#define KPROBE_LOOKUP 1
extern void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
		                unsigned long end, unsigned int stride_shift,
				                bool freed_tables);
#include <asm/tlb.h>
#include <linux/kprobes.h>
static struct kprobe kp = {
	.symbol_name = "kallsyms_lookup_name"
};
#endif

/* ----- local variable ----- */
static struct k_symbol	*ksym;
/* ----- local variable ----- */

/**
 * kernel_symbol_init - Initialize dynamic kernel symbol lookup
 *
 * For using kernel symbols, this function makes a lookup table for needed symbols
 */
int kernel_symbol_init(void) {
	int ret = 0; // number of not found symbols
#ifdef KPROBE_LOOKUP
	typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
	kallsyms_lookup_name_t kallsyms_lookup_name;
#endif

	// Check if already did. If this failed at the first time, The whole module init was failed and there is no chance to the second try.
	if (ksym)
		return 0;

#ifdef KPROBE_LOOKUP
	register_kprobe(&kp);
	kallsyms_lookup_name = (kallsyms_lookup_name_t) kp.addr;
	unregister_kprobe(&kp);
#endif

	ksym = emp_kmalloc(sizeof(struct k_symbol), GFP_KERNEL);
	if(!ksym) {
		printk(KERN_ERR "failed to allocate memory for ksym\n");
		return -ENOMEM;
	}
	memset(ksym, 0, sizeof(struct k_symbol));

#define KALLSYM_LOOKUP(ksym, name, ret) do { \
	(ksym)->name = kallsyms_lookup_name(#name); \
	if ((ksym)->name == (unsigned long) NULL) { \
		(ret)++; \
		printk(KERN_ERR "ERROR: %s failed to find symbol '" #name "'\n", __func__); \
	} \
} while (0)

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(10, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0))
	KALLSYM_LOOKUP(ksym, page_add_file_rmap, ret);
	KALLSYM_LOOKUP(ksym, page_remove_rmap, ret);
#else // RHEL_RELEASE_VERSION >= 9.8 or KERNEL_VERSION >= 6.8
	KALLSYM_LOOKUP(ksym, folio_add_file_rmap_ptes, ret);
	KALLSYM_LOOKUP(ksym, folio_remove_rmap_ptes, ret);
#endif

	KALLSYM_LOOKUP(ksym, tlb_finish_mmu, ret);
	KALLSYM_LOOKUP(ksym, tlb_gather_mmu, ret);
	KALLSYM_LOOKUP(ksym, pte_alloc_one, ret);
	KALLSYM_LOOKUP(ksym, ptep_clear_flush, ret);
	KALLSYM_LOOKUP(ksym, thread_group_cputime_adjusted, ret);
	KALLSYM_LOOKUP(ksym, sysctl_hung_task_timeout_secs, ret);
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 5)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
	KALLSYM_LOOKUP(ksym, blkdev_get_no_open, ret);
#endif

	return ret;
}

/**
 * kernel_symbol_init - Disable dynamic kernel symbol lookup
 */
void kernel_symbol_close(void) {
	if(ksym) {
		emp_kfree(ksym);
		ksym = NULL;
	}
}

#ifndef CONFIG_EMP_PAGE_RMAP_OPT
void kernel_page_add_file_rmap(struct page *page, struct vm_area_struct *vma, bool compound)
{
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0))
	void (*f)(struct page *, struct vm_area_struct *vma, bool compound);
	f = (void (*)(struct page *, struct vm_area_struct *vma, bool compound))
			ksym->page_add_file_rmap;
	f(page, vma, compound);
#elif (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(10, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0))
	void (*f)(struct page *, struct vm_area_struct *vma, bool compound);
	f = (void (*)(struct page *, struct vm_area_struct *vma, bool compound))
			ksym->page_add_file_rmap;
	f(page, vma, compound);
#else
	void (*f)(struct folio *folio, struct page *page,
			int nr_pages, struct vm_area_struct *vma);
	struct folio *folio;
	unsigned int nr_pages;
	f = (void (*)(struct folio *folio, struct page *page,
			int nr_pages, struct vm_area_struct *vma))
				ksym->folio_add_file_rmap_ptes;
	folio = page_folio(page);
	if (!compound)
		nr_pages = 1;
	else
		nr_pages = folio_nr_pages(folio);
	f(folio, page, nr_pages, vma);
#endif
}

void kernel_page_remove_rmap(struct page *pg, struct vm_area_struct *vma, bool compound)
{
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0))
	void (*f)(struct page *, bool compound);
	f = (void (*)(struct page *, bool compound))
			ksym->page_remove_rmap;
	f(pg, compound);
#elif (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(10, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0))
	void (*f)(struct page *, struct vm_area_struct *vma, bool compound);
	f = (void (*)(struct page *, struct vm_area_struct *vma, bool compound))
			ksym->page_remove_rmap;
	f(pg, vma, compound);
#else
	void (*f)(struct folio *folio, struct page *page,
			int nr_pages, struct vm_area_struct *vma);
	struct folio *folio;
	unsigned int nr_pages;
	f = (void (*)(struct folio *folio, struct page *page,
			int nr_pages, struct vm_area_struct *vma))
				ksym->folio_remove_rmap_ptes;
	folio = page_folio(pg);
	if (!compound)
		nr_pages = 1;
	else
		nr_pages = folio_nr_pages(folio);
	f(folio, pg, nr_pages, vma);
#endif
}
#endif /* CONFIG_EMP_PAGE_RMAP_OPT */

void kernel_tlb_finish_mmu(struct mmu_gather *tlb, unsigned long start,
		unsigned long end)
{
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0))
	void (*f)(struct mmu_gather *tlb);

	f = (void (*)(struct mmu_gather *tlb))
		ksym->tlb_finish_mmu;
	f(tlb);
#else
	void (*f)(struct mmu_gather *tlb, unsigned long start,
			unsigned long end);

	f = (void (*)(struct mmu_gather *tlb, unsigned long start,
				unsigned long end))
		ksym->tlb_finish_mmu;
	f(tlb, start, end);
#endif
}

void kernel_tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm,
			unsigned long start, unsigned long end)
{
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0))
	void (*f)(struct mmu_gather *tlb, struct mm_struct *mm);
	f = (void (*)(struct mmu_gather *tlb, struct mm_struct *mm))
			ksym->tlb_gather_mmu;
	f(tlb, mm);
#else
	void (*f)(struct mmu_gather *tlb, struct mm_struct *mm,
			unsigned long start, unsigned long end);
	f = (void (*)(struct mmu_gather *tlb, struct mm_struct *mm,
				unsigned long start, unsigned long end))
		ksym->tlb_gather_mmu;
	f(tlb, mm, start, end);
#endif
}

/*
 * following definition came from mm/huge_memory.c
 * it's for transparent_hugepage_defrag.
 */
unsigned long transparent_hugepage_flags __read_mostly =
#ifdef CONFIG_TRANSPARENT_HUGEPAGE_ALWAYS
	(1<<TRANSPARENT_HUGEPAGE_FLAG)|
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE_MADVISE
	(1<<TRANSPARENT_HUGEPAGE_REQ_MADV_FLAG)|
#endif
	(1<<TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG)|
	(1<<TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG)|
	(1<<TRANSPARENT_HUGEPAGE_USE_ZERO_PAGE_FLAG);

void kernel_thread_group_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	void (*f)(struct task_struct *p, u64 *ut, u64 *st);
	f = (void (*)(struct task_struct *p, u64 *ut, u64 *st))
		ksym->thread_group_cputime_adjusted;
	return f(p, ut, st);
}

unsigned long kernel_sysctl_hung_task_timeout_secs(void)
{
	return *(unsigned long *)ksym->sysctl_hung_task_timeout_secs;
}

pgtable_t kernel_pte_alloc_one(struct mm_struct *mm, unsigned long address)
{
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0))
	pgtable_t (*f)(struct mm_struct *);
	f = (pgtable_t (*)(struct mm_struct *))ksym->pte_alloc_one;
	return f(mm);
#else
	pgtable_t (*f)(struct mm_struct *, unsigned long);
	f = (pgtable_t (*)(struct mm_struct *, unsigned long))ksym->pte_alloc_one;
	return f(mm, address);
#endif
}

pte_t kernel_ptep_clear_flush(struct vm_area_struct *vma, unsigned long address, pte_t *ptep)
{
	pte_t (*f)(struct vm_area_struct *, unsigned long, pte_t *);
	f = (pte_t (*)(struct vm_area_struct *, unsigned long, pte_t *))ksym->ptep_clear_flush;
	return f(vma, address, ptep);
}

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 5)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
struct block_device *kernel_blkdev_get_no_open(dev_t dev)
{
	struct block_device *(*f)(dev_t);
	f = (struct block_device *(*)(dev_t))ksym->blkdev_get_no_open;
	if (!f)
		return ERR_PTR(-ENOENT);
	return f(dev);
}
#endif
