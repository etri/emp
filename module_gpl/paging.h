#ifndef __PAGING_H__
#define __PAGING_H__
#include "vm.h"

#define GPA_TO_GFN(gpa) ((gpa) >> PAGE_SHIFT)
#define GFN_TO_GPA(gpn) ((gpn) << PAGE_SHIFT)
#define HVA_TO_GPN      __hva_to_gfn
#define GPN_OFFSET      __gpn_offset
#define GPN_OFFSET_TO_HVA __gfn_offset_to_hva
#define GPN_VMA_LEN(vma) GPA_TO_GFN(vma->vm_end - vma->vm_start)

#define VA_ROUND_DOWN_ORDER(v, order) \
	((v) & ~((1 << (order)) - 1))
#define VA_ROUND_UP_ORDER(v, order) \
	VA_ROUND_DOWN_ORDER((v - 1) + (1 << (order)), order)

#define set_pg_mlocked(p) { SetPageMlocked(p); }
#define clear_pg_mlocked(p) { ClearPageMlocked(p); }
#define clear_page_state(p) do { p->flags &= ~(PAGE_FLAGS_CHECK_AT_PREP & ~PG_head_mask); } while (0) // clear PAGE_FLAGS_CHECK_AT_PREP but keeps PG_head_mask

static inline unsigned long 
__hva_to_gfn(struct emp_mm *emm, struct emp_vmr *vmr, unsigned long hva)
{
#ifdef CONFIG_EMP_VM
	int i;
	unsigned long start, end;

	if (emm->ekvm.memslot_len == 0)
		return GPA_TO_GFN(hva - vmr->vm_start);
	FOR_EACH_MEMSLOT(emm, i) {
		start = emm->ekvm.memslot[i].hva;
		end = start + emm->ekvm.memslot[i].size;
		if (hva < start || hva >= end)
			continue;
		return GPA_TO_GFN(hva - start + emm->ekvm.memslot[i].gpa);
	}
	BUG();
#else
	return GPA_TO_GFN(hva - vmr->vm_start);
#endif
}

#ifdef CONFIG_EMP_VM
static inline unsigned long 
__gfn_to_hva(struct emp_mm *bvma, struct emp_vmr *vmr, unsigned long gfn)
{
	int i;
	unsigned long start, end;
	unsigned long gpa = GFN_TO_GPA(gfn);
	unsigned long addr;

	if (bvma->ekvm.memslot_len == 0)
		return (vmr->vm_start + gpa);
	FOR_EACH_MEMSLOT(bvma, i) {
		start = bvma->ekvm.memslot[i].gpa;
		end = start + bvma->ekvm.memslot[i].size;
		if (gpa < start || gpa >= end)
			continue;
		addr = ((gpa - start) + bvma->ekvm.memslot[i].hva);
		return addr;
	}
	BUG();
}
#endif

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

#ifdef CONFIG_EMP_USER
/* Pages of @g which @vmr can see, and the page of @g they start at.
 *
 * @hva is the address of @g itself and is NOT moved: every index in this
 * machinery -- gpa_index, GPN_OFFSET_TO_HVA(), the pgoff the fault path derives
 * as (address - descs->vm_base) >> PAGE_SHIFT, and so demand_offset -- counts
 * from the subblock's own start, so the address of a run must too. A mapping
 * which begins inside the subblock is reported as @off, and the caller adds it
 * where it needs the first mapped address.
 */
#define ____partial_gpa_len_off(vmr, g, idx, hva, len, off) do { \
	unsigned long ____end = (hva) + ((1 << PAGE_SHIFT) << gpa_subblock_order(g)); \
	debug_assert(gpa_subblock_order(g) == gpa_block_order(g)); \
	(len) = 1 << gpa_subblock_order(g); \
	(off) = 0; \
	if ((hva) < (vmr)->vm_start) { \
		(off) = (unsigned int) (((vmr)->vm_start - (hva)) >> PAGE_SHIFT); \
		(len) -= (off); \
	} \
	if (____end > (vmr)->vm_end) \
		(len) -= (____end - (vmr)->vm_end) >> PAGE_SHIFT; \
} while (0)

#define ____gpa_to_hva_len_off(vmr, g, idx, hva, len, off) do { \
	(hva) = GPN_OFFSET_TO_HVA(vmr, idx, gpa_subblock_order(g)); \
	if (likely(!__is_gpa_flags_set(g, GPA_PARTIAL_MAP_MASK))) { \
		(len) = 1 << gpa_subblock_order(g); \
		(off) = 0; \
	} else \
		____partial_gpa_len_off(vmr, g, idx, hva, len, off); \
} while (0)

#define ____local_gpa_to_hva_len_off(vmr, g, hva, len, off) \
	____gpa_to_hva_len_off(vmr, g, (g)->local_page->gpa_index, hva, len, off)
#else /* !CONFIG_EMP_USER */
#define ____gpa_to_hva_len_off(vmr, g, idx, hva, len, off) do { \
	(hva) = GPN_OFFSET_TO_HVA(vmr, idx, gpa_subblock_order(g)); \
	(len) = 1 << gpa_subblock_order(g); \
	(off) = 0; \
} while (0)

#define ____local_gpa_to_hva_len_off(vmr, g, hva, len, off) \
	____gpa_to_hva_len_off(vmr, g, (g)->local_page->gpa_index, hva, len, off)
#endif /* !CONFIG_EMP_USER */

#ifdef CONFIG_EMP_USER
static inline int
__gpa_to_page_len(struct emp_vmr *vmr, struct emp_gpa *gpa, unsigned long idx)
{
	unsigned long hva;
	unsigned int len, off;

	if (likely(!__is_gpa_flags_set(gpa, GPA_PARTIAL_MAP_MASK)))
		return 1 << gpa_subblock_order(gpa);
	____gpa_to_hva_len_off(vmr, gpa, idx, hva, len, off);
	return len;
}

/* the first address of @g which @vmr maps */
#define local_gpa_to_hva(vmr, g) ({ \
	unsigned long ____hva; \
	unsigned int ____len, ____off; \
	____local_gpa_to_hva_len_off(vmr, g, ____hva, ____len, ____off); \
	____hva + ((unsigned long) ____off << PAGE_SHIFT); \
})
#else /* !CONFIG_EMP_USER */
static inline int
__gpa_to_page_len(struct emp_vmr *vmr, struct emp_gpa *gpa, unsigned long idx)
{
	return 1 << gpa_subblock_order(gpa);
}

#define local_gpa_to_hva(vmr, g) GPN_OFFSET_TO_HVA(vmr, \
				(g)->local_page->gpa_index, gpa_subblock_order(g))
#endif /* !CONFIG_EMP_USER */

static inline int
__local_gpa_to_page_len(struct emp_vmr *vmr, struct emp_gpa *gpa)
{
	debug_assert(gpa->local_page);
	return __gpa_to_page_len(vmr, gpa, gpa->local_page->gpa_index);
}

#ifdef CONFIG_EMP_USER
static inline int
__local_block_to_page_len(struct emp_vmr *vmr, struct emp_gpa *head)
{
	debug_assert(head->local_page);
	if (likely(!__is_gpa_flags_set(head, GPA_PARTIAL_MAP_MASK)))
		return gpa_block_size(head);
	else {
		unsigned long hva;
		unsigned int len, off;
		____local_gpa_to_hva_len_off(vmr, head, hva, len, off);
		return len;
	}
}
#else /* !CONFIG_EMP_USER */
static inline int
__local_block_to_page_len(struct emp_vmr *vmr, struct emp_gpa *head)
{
	debug_assert(head->local_page);
	return gpa_block_size(head);
}
#endif /* !CONFIG_EMP_USER */

static inline void
____emp_get_pages_map(struct emp_gpa *gpa, unsigned long page_len)
{
	struct page *page = gpa->local_page->page;
	debug_check_head(page);
	page_ref_add(page, page_len);
	gpa->local_page->page_map_count += page_len;
}

#define __emp_get_pages_map(vmr, gpa, page_len) do { \
	____emp_get_pages_map(gpa, page_len); \
	debug_page_ref_mark(emp_vmr_dbgid(vmr), (gpa)->local_page, page_len); \
} while (0)

#define emp_get_pages_map(vmr, gpa, idx) do { \
	unsigned long ____hva; \
	unsigned int ____page_len, ____off; \
	____gpa_to_hva_len_off(vmr, gpa, idx, ____hva, ____page_len, ____off); \
	____emp_get_pages_map(gpa, ____page_len); \
	debug_page_ref_mark(emp_vmr_dbgid(vmr), (gpa)->local_page, ____page_len); \
} while (0)

static inline void
____emp_put_pages_map(struct emp_gpa *gpa, unsigned long page_len)
{
	struct page *page = gpa->local_page->page;
	debug_check_head(page);
	page_ref_sub(page, page_len);
	gpa->local_page->page_map_count -= page_len;
}

#define __emp_put_pages_map(vmr, gpa, page_len) do { \
	____emp_put_pages_map(gpa, page_len); \
	debug_page_ref_mark(emp_vmr_dbgid(vmr), (gpa)->local_page, -page_len); \
} while (0)

#define emp_put_pages_map(vmr, gpa, idx) do { \
	unsigned long ____hva; \
	unsigned int ____page_len, ____off; \
	____gpa_to_hva_len_off(vmr, gpa, idx, ____hva, ____page_len, ____off); \
	____emp_put_pages_map(gpa, ____page_len); \
	debug_page_ref_mark(emp_vmr_dbgid(vmr), (gpa)->local_page, -____page_len); \
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

/* An EMP vma is identified by its vm_ops. NULL for a foreign vma, and for an
 * EMP vma whose vmr creation failed. */
static inline struct emp_vmr *__get_emp_vmr(struct vm_area_struct *vma)
{
	if (unlikely(vma->vm_ops != &emp_vma_ops))
		return NULL;
	return (struct emp_vmr *)vma->vm_private_data;
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
