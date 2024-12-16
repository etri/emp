#ifdef CONFIG_EMP_VM
#ifndef __MMU_H__
#define __MMU_H__

#define SHADOW_DIRTY_SHIFT (9)
#define SHADOW_DIRTY_MASK  (1 << SHADOW_DIRTY_SHIFT)
#define SHADOW_ACCESSED_SHIFT (8)
#define SHADOW_ACCESSED_MASK  (1 << SHADOW_ACCESSED_SHIFT) 
#define PT64_BASE_ADDR_MASK (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE-1))
#define SHADOW_EPT_MASK (PT64_BASE_ADDR_MASK | SHADOW_DIRTY_MASK | \
		SHADOW_ACCESSED_MASK)

#define INVALID_PAGE (~(hpa_t)0)
#define VALID_PAGE(x) ((x) != INVALID_PAGE)

#define PTE_PREFETCH_NUM (8)

enum {
	EMP_PF_RETRY = 0,
	EMP_PF_RETURN_ESC_LEVEL = 1,
	EMP_PF_REACH_REQ_LEVEL = 2,
};

// extended in kvm/mmu.c
bool kvm_emp_is_mmio_spte(u64 spte);
bool kvm_emp_is_accessed_spte(u64 spte);
void kvm_emp_walk_shadow_page_lockless_begin(struct kvm_vcpu *vcpu);
void kvm_emp_walk_shadow_page_lockless_end(struct kvm_vcpu *vcpu);
int kvm_emp_mmu_topup_memory_caches(struct kvm_vcpu *vcpu);
int kvm_emp_kvm_unmap_hva_range(struct kvm *kvm, unsigned long start, unsigned long end);
int kvm_emp_make_mmu_pages_available(struct kvm_vcpu *vcpu);
void kvm_emp_mmu_rmap_add(struct kvm_vcpu *vcpu, struct kvm_memory_slot *ms,
				u64 *sptep, gfn_t gfn, unsigned int pte_access);
int kvm_emp_mmu_set_spte(struct kvm_vcpu *vcpu, u64 *sptep,
			unsigned int pte_access, bool write_fault, int level,
			gfn_t gfn, kvm_pfn_t pfn, bool speculative,
			bool host_writable);
u64 *kvm_emp___get_spte(struct kvm_vcpu *vcpu, int level, gfn_t gfn);
int kvm_emp___prepare_map(struct kvm_vcpu *vcpu, int *level, gpa_t gpa,
		kvm_pfn_t *pfn, bool tdp_and_huge_page_disallowed,
		gfn_t *base_gfn, u64 **sptep);
int kvm_emp___direct_map(struct kvm_vcpu *vcpu, gpa_t gpa, int write,
		int map_writable, int max_level, kvm_pfn_t pfn, bool prefault,
		struct kvm_memory_slot *slot);

unsigned kvm_emp_get_shadow_page_level(hpa_t shadow_page);

bool kvm_emp_fast_page_fault(struct kvm_vcpu *vcpu, gpa_t cr2_or_gpa, int level, u32 error_code);

#endif /* __MMU_H__ */
#endif /* CONFIG_EMP_VM */
