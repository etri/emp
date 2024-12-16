#ifndef __GLUE_H__
#define __GLUE_H__

#include <linux/version.h>
#include <compat.h>

#ifdef CONFIG_EMP_VM
#include <linux/kvm_host.h>
#include <mmu.h>
#include <linux/kvm_host.h>
#include <asm/vmx.h>

// from c++17, __has_include supported to check the header's existence
#ifdef __has_include
#  if __has_include(<vmx/ops.h>)
#    include <vmx/ops.h>
#  endif
#endif

#define PT_WRITABLE_SHIFT 1
#define PT_WRITABLE_MASK (1ULL << PT_WRITABLE_SHIFT)
#define ACC_EXEC_MASK    1
#define ACC_WRITE_MASK   PT_WRITABLE_MASK
#define ACC_USER_MASK    PT_USER_MASK
#define ACC_ALL          (ACC_EXEC_MASK | ACC_WRITE_MASK | ACC_USER_MASK)
#endif /* CONFIG_EMP_VM */

void kernel_page_add_file_rmap(struct page *page, struct vm_area_struct *vma, bool compound);
void kernel_page_remove_rmap(struct page *pg, struct vm_area_struct *vma, bool compound);
void kernel_tlb_finish_mmu(struct mmu_gather *tlb, unsigned long start, unsigned long end);
void kernel_tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm,
			unsigned long start, unsigned long end);
void kernel_thread_group_cputime_adjusted(struct task_struct *, u64 *, u64 *);
unsigned long kernel_sysctl_hung_task_timeout_secs(void);
pgtable_t kernel_pte_alloc_one(struct mm_struct *mm, unsigned long address);
pte_t kernel_ptep_clear_flush(struct vm_area_struct *vma, unsigned long address, pte_t *ptep);

struct k_symbol {
	unsigned long page_add_file_rmap;
	unsigned long page_remove_rmap;
	unsigned long tlb_finish_mmu;
	unsigned long tlb_gather_mmu;
	unsigned long pte_alloc_one;
	unsigned long thread_group_cputime_adjusted;
	unsigned long sysctl_hung_task_timeout_secs;
	unsigned long ptep_clear_flush;
};

int kernel_symbol_init(void);
void kernel_symbol_close(void);

#endif /* __GLUE_H__ */
