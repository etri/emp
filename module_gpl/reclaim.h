#ifndef __RECLAIM_H__
#define __RECLAIM_H__

#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/kvm_host.h>
#include "vm.h"
#include "block.h"
#include "vcpu_var.h"
#include "udma.h"

// currently 2MB/thread is allocated for headroom
#define LOCAL_CACHE_BUFFER_SIZE(b) \
	((2 * EMP_MAIN_CPU_LEN(b)) << (MB_ORDER - PAGE_SHIFT))
#define LOCAL_CACHE_MAX(b) \
	(atomic_read(&((b)->ftm.local_cache_pages)) + \
	  b->ftm.local_cache_pages_headroom)

#define IS_PAGE_IMBALANCE(bvma, c) \
	((atomic_read(&(bvma)->ftm.alloc_pages_len) >= LOCAL_CACHE_MAX(bvma)) && \
		atomic_read(&(bvma)->ftm.free_pages.len) < BLOCK_MAX_SIZE && \
		bvma->vcpus[(c)].local_free_pages_len < BLOCK_MAX_SIZE)

#ifdef CONFIG_EMP_VM
static inline void lock_kvm_mmu_lock(struct kvm *kvm) {
	if (!kvm)
		return;
#ifdef KVM_HAVE_MMU_RWLOCK
	write_lock(&kvm->mmu_lock);
#else
	spin_lock(&kvm->mmu_lock);
#endif
}

static inline void unlock_kvm_mmu_lock(struct kvm *kvm) {
	if (!kvm)
		return;
#ifdef KVM_HAVE_MMU_RWLOCK
	write_unlock(&kvm->mmu_lock);
#else
	spin_unlock(&kvm->mmu_lock);
#endif
}
#endif /* CONFIG_EMP_VM */

/* Lock Acquisition Order (active list)
 *  lru -> mru
 *  lru -> global
 *  mru -> global
 *
 * To prevent deadlock.
 */

#define MRU_QUEUE_LEVEL (6) // confirmed that minimum is 5, max 7
#define MRU_QUEUE_DEPTH (1 << MRU_QUEUE_LEVEL)
#define LRU_QUEUE_LEVEL (6) // confirmed that minimum is 4, max 9
#define LRU_QUEUE_DEPTH (1 << LRU_QUEUE_LEVEL)
#define PROMOTES_SIZE	(16)

int update_lru_lists(struct emp_mm *, struct vcpu_var *, struct emp_gpa **, int,
		     int);
int update_lru_lists_reref(struct emp_mm *, struct vcpu_var *,
			   struct emp_gpa **, int, int);
int update_lru_lists_lru(struct emp_mm *, struct vcpu_var *, struct emp_gpa **,
			 int, int);
int add_gpas_to_active_list(struct emp_mm *, struct vcpu_var *,
			    struct emp_gpa **, int);
int add_gpas_to_inactive(struct emp_mm *bvma, struct vcpu_var *cpu,
				struct emp_gpa **gpas, int n_new);
int emp_writeback_block(struct emp_mm *, struct emp_gpa *, struct vcpu_var *);
int reclaim_gpa(struct emp_mm *, struct emp_gpa *, bool *);
int reclaim_emp_pages(struct emp_mm *, struct vcpu_var *, int, bool);
void remove_gpa_from_lru(struct emp_mm *emm, struct emp_gpa *head);
void reclaim_set(struct emp_mm *);
int reclaim_init(struct emp_mm *);
void reclaim_exit(struct emp_mm *);

static inline void  
wait_for_prefetch_subblocks(struct emp_mm *emm, struct vcpu_var *cpu,
			    struct emp_gpa *vs[], int n_vs) {}

bool check_gpa_block_reclaimable(struct emp_mm *, struct emp_gpa *);

/**
 * is_unmapped_active - Check the pages status (unmapped and active)
 * @param gpa gpa of page
 *
 * @retval true: unmapped & active (currently fetching)
 * @retval false: not (unmapped & active)
 */
static inline bool is_unmapped_active(struct emp_gpa *gpa)
{
	return (gpa->r_state == GPA_FETCHING);
}

#define ROUND_UP(x,y) (((x)+(y)-1)/(y))

static inline void list_del_safe(struct list_head *list) {
	if (unlikely(list_empty(list)))
		return;
	if (unlikely(list->prev == LIST_POISON2
			&& list->next == LIST_POISON1))
		return;
	list_del(list);
}

static inline int PTR_ERR_RET(const void *ptr) {
	if (likely(IS_ERR(ptr) == false))
		return ptr ? 1 : 0;
	else
		return (int) PTR_ERR(ptr);
}

static inline void link_entangled_writeback(struct work_request *current_wr,
					    struct work_request *head_wr,
					    struct work_request **tail_wr,
					    int *ew_len)
{
	if (head_wr)
		list_add_tail(&(current_wr->remote_pages),
			      &(head_wr->remote_pages));
	*tail_wr = current_wr;
	(*ew_len)++;
}

#endif /* __RECLAIM_H__ */
