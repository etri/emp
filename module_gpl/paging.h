#ifndef __PAGING_H__
#define __PAGING_H__
#include "vm.h"

#define IS_INVALID_HVA(emm, va) (emp_vmr_lookup_hva(emm, va) == NULL)
#define GPA_TO_GFN(gpa) ((gpa) >> PAGE_SHIFT)
#define GFN_TO_GPA(gpn) ((gpn) << PAGE_SHIFT)
#define HVA_TO_GPN      __hva_to_gfn
#define GPN_TO_HVA      __gfn_to_hva
#define GPN_OFFSET      __gpn_offset
#define GPN_OFFSET_TO_HVA __gfn_offset_to_hva
#define GPN_VMA_LEN(vma) GPA_TO_GFN(vma->vm_end - vma->vm_start)

#define VA_ROUND_DOWN_ORDER(v, order) \
	((v) & ~((1 << (order)) - 1))
#define VA_ROUND_UP_ORDER(v, order) \
	VA_ROUND_DOWN_ORDER((v - 1) + (1 << (order)), order)

#define set_pg_mlocked(p) { SetPageMlocked(p); }
#define clear_pg_mlocked(p) { ClearPageMlocked(p); }

static inline unsigned long 
__hva_to_gfn(struct emp_mm *emm, struct emp_vmr *vmr, unsigned long hva)
{
#ifdef CONFIG_EMP_VM
	int i;
	unsigned long start, end;

	if (emm->ekvm.memslot_len == 0)
		return GPA_TO_GFN(hva - vmr->host_vma->vm_start);
	FOR_EACH_MEMSLOT(emm, i) {
		start = emm->ekvm.memslot[i].hva;
		end = start + emm->ekvm.memslot[i].size;
		if (hva < start || hva >= end)
			continue;
		return GPA_TO_GFN(hva - start + emm->ekvm.memslot[i].gpa);
	}
	BUG();
#else
	return GPA_TO_GFN(hva - vmr->host_vma->vm_start);
#endif
}

static inline unsigned long 
__gfn_to_hva(struct emp_mm *bvma, struct emp_vmr *vmr, unsigned long gfn)
{
#ifdef CONFIG_EMP_VM
	int i;
	unsigned long start, end;
	unsigned long gpa = GFN_TO_GPA(gfn);
	unsigned long addr;

	if (bvma->ekvm.memslot_len == 0)
		return (vmr->host_vma->vm_start + gpa);
	FOR_EACH_MEMSLOT(bvma, i) {
		start = bvma->ekvm.memslot[i].gpa;
		end = start + bvma->ekvm.memslot[i].size;
		if (gpa < start || gpa >= end)
			continue;
		addr = ((gpa - start) + bvma->ekvm.memslot[i].hva);
		debug_BUG_ON(IS_INVALID_HVA(bvma, addr));
		return addr;
	}
	BUG();
#else
	unsigned long gpa = GFN_TO_GPA(gfn);
	return (vmr->host_vma->vm_start + gpa);
#endif
}

static inline unsigned long 
__gfn_offset_to_hva(struct emp_vmr *vmr, unsigned long gpn_offset, int sb_order)
{
	return (vmr->descs->vm_base + GFN_TO_GPA(gpn_offset << sb_order));
}

static inline unsigned long __gpn_offset(struct emp_mm *bvma, unsigned long gpn)
{
#ifdef CONFIG_EMP_VM
	int i;
	unsigned long start, end;

	if (bvma->ekvm.memslot_len == 0)
		return gpn;
	FOR_EACH_MEMSLOT(bvma, i) {
		start = GPA_TO_GFN(bvma->ekvm.memslot[i].gpa);
		end = start + GPA_TO_GFN(bvma->ekvm.memslot[i].size);
		if (gpn < start || gpn >= end)
			continue;
		return (gpn - start + bvma->ekvm.memslot[i].base);
	}
	BUG();
#else
	return gpn;
#endif
}

// get hva and number of pages with consideration of partial map
#define ____gpa_to_hva_and_len(vmr, g, idx, hva, len) do { \
	(hva) = GPN_OFFSET_TO_HVA(vmr, idx, (g)->sb_order); \
	(len) = 1 << gpa_subblock_order(g); \
} while (0)

#define ____local_gpa_to_hva_and_len(vmr, g, hva, len) do { \
	(hva) = GPN_OFFSET_TO_HVA(vmr, (g)->local_page->gpa_index, \
							(g)->sb_order); \
	(len) = 1 << gpa_subblock_order(g); \
} while (0)

static inline int
__gpa_to_page_len(struct emp_vmr *vmr, struct emp_gpa *gpa, unsigned long idx)
{
	return 1 << gpa_subblock_order(gpa);
}

static inline int
__local_gpa_to_page_len(struct emp_vmr *vmr, struct emp_gpa *gpa)
{
	debug_assert(gpa->local_page);
	return __gpa_to_page_len(vmr, gpa, gpa->local_page->gpa_index);
}

static inline int
__local_block_to_page_len(struct emp_vmr *vmr, struct emp_gpa *head)
{
	debug_assert(head->local_page);
	return gpa_block_size(head);
}

static inline void
____emp_get_pages_map(struct emp_gpa *gpa, unsigned long page_len)
{
	struct page *page = gpa->local_page->page;

	if (PageCompound(page)) {
		page = compound_head(page);
		page_ref_add(page, page_len);
		return;
	}

	while (page_len-- > 0)
		get_page(page++);
}

#define __emp_get_pages_map(vmr, gpa, page_len) do { \
	____emp_get_pages_map(gpa, page_len); \
	debug_page_ref_mark((vmr)->id, (gpa)->local_page, page_len); \
} while (0)

#define emp_get_pages_map(vmr, gpa, idx) do { \
	unsigned long ____hva, ____page_len; \
	____gpa_to_hva_and_len(vmr, gpa, idx, ____hva, ____page_len); \
	____emp_get_pages_map(gpa, ____page_len); \
	debug_page_ref_mark((vmr)->id, (gpa)->local_page, ____page_len); \
} while (0)

static inline void
____emp_put_pages_map(struct emp_gpa *gpa, unsigned long page_len)
{
	struct page *page = gpa->local_page->page;

	if (PageCompound(page)) {
		page = compound_head(page);
		page_ref_sub(page, page_len);
		return;
	}

	while (page_len-- > 0)
		put_page(page++);
}

#define __emp_put_pages_map(vmr, gpa, page_len) do { \
	____emp_put_pages_map(gpa, page_len); \
	debug_page_ref_mark((vmr)->id, (gpa)->local_page, -page_len); \
} while (0)

#define emp_put_pages_map(vmr, gpa, idx) do { \
	unsigned long ____hva, ____page_len; \
	____gpa_to_hva_and_len(vmr, gpa, idx, ____hva, ____page_len); \
	____emp_put_pages_map(gpa, ____page_len); \
	debug_page_ref_mark((vmr)->id, (gpa)->local_page, -____page_len); \
} while (0)

static inline unsigned long get_page_mask(struct emp_mm *bvma)
{
	return ~(PAGE_SIZE - 1);
}

#ifdef CONFIG_EMP_VM
// spte_to_pfn is from kvm/mmu.c
#define PT64_BASE_ADDR_MASK (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE-1))
static inline u64 spte_to_pfn(u64 pte)
{
	return (pte & PT64_BASE_ADDR_MASK) >> PAGE_SHIFT;
}
#endif

static inline struct emp_vmr *__get_emp_vmr(struct vm_area_struct *vma)
{
	return (struct emp_vmr *)vma->vm_private_data;
}

// _get_emp_mm does not need to put_emp_mm
static inline struct emp_mm *__get_emp_mm(struct vm_area_struct *vma)
{
	extern struct emp_mm **emp_mm_arr;
	extern unsigned long emp_mm_arr_len;
	extern spinlock_t emp_mm_arr_lock;

	int i;
	struct emp_mm *emm;

	if (vma->vm_private_data)
		return __get_emp_vmr(vma)->emm;

	emm = NULL;
	spin_lock(&emp_mm_arr_lock);
	for (i = 0; i < emp_mm_arr_len; i++) {
		if (emp_vmr_lookup(emp_mm_arr[i], vma) != NULL) {
			emm = emp_mm_arr[i];
			break;
		}
	}
	spin_unlock(&emp_mm_arr_lock);

	return emm;
}

static inline struct emp_mm *get_emp_mm(int id)
{
	extern struct emp_mm **emp_mm_arr;
	extern spinlock_t emp_mm_arr_lock;
	struct emp_mm *bvma = NULL;

	if (id < 0 || id >= EMP_MM_MAX)
		return NULL;

	spin_lock(&emp_mm_arr_lock);
	bvma = emp_mm_arr[id];
	spin_unlock(&emp_mm_arr_lock);

	if (bvma) {
		atomic_inc(&bvma->refcount);
		printk(KERN_ERR "bvma:id:%d refcount:%d\n", bvma->id, 
				atomic_read(&bvma->refcount));
	}
	return bvma;
}

static inline void put_emp_mm(struct emp_mm *emm)
{
	bool zero;
	zero = atomic_dec_and_test(&emm->refcount);
	WARN_ON(zero == true);
	printk(KERN_ERR "emm:id:%d refcount:%d\n", emm->id, 
			atomic_read(&emm->refcount));
}
#endif /* __PAGING_H__ */
