#ifndef __EMP_H__
#define __EMP_H__

#include <linux/version.h>
#include <linux/scatterlist.h>
#include <linux/pagemap.h>
#include <linux/kobject.h>
#include "config.h"
#include "emp_type.h"
#include "constants.h"
#include "lru.h"
#include "vcpu_var.h"
#include "../include/emp_ioctl.h"
#include "../include/compat.h"
#include "debug.h"

#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
extern atomic64_t num_emp_gpa_fault;
extern atomic64_t num_emp_hva_fault;
#define CONFIG_EMP_SHOW_FAULT_PROGRESS_PERIOD (10000000)
#endif

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0))
#define KPROBE_LOOKUP 1
extern void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
		                unsigned long end, unsigned int stride_shift,
				                bool freed_tables);
#include <asm/tlb.h>
#include <compat.h>
#else
#include <asm/tlb.h>
#endif

#ifdef CONFIG_EMP_DEBUG_RSS
enum DEBUG_RSS_ADD_ID {
	DEBUG_RSS_ADD_INSTALL_HPTES,
	DEBUG_RSS_ADD_WRITEBACK_CURR,
	DEBUG_RSS_ADD_INACTIVE_CURR,
	DEBUG_RSS_ADD_WRITEBACK_CURR_PRO,
	DEBUG_RSS_ADD_INACTIVE_CURR_PRO,
	DEBUG_RSS_ADD_FAULT_GPA,
	DEBUG_RSS_ADD_ALLOC_FETCH,
	DEBUG_RSS_ADD_PUT_MAX_BLOCK,
	DEBUG_RSS_ADD_COW_OTHER_ACTIVE,
	DEBUG_RSS_ADD_COW_INACTIVE,
	DEBUG_RSS_ADD_COW_WRITEBACK,
	NUM_DEBUG_RSS_ADD_ID,
};

enum DEBUG_RSS_SUB_ID {
	DEBUG_RSS_SUB_WRITEBACK_PREV,
	DEBUG_RSS_SUB_INACTIVE_PREV,
	DEBUG_RSS_SUB_WRITEBACK_PREV_PRO,
	DEBUG_RSS_SUB_INACTIVE_PREV_PRO,
	DEBUG_RSS_SUB_UNMAP_PTES,
	DEBUG_RSS_SUB_UNMAP_GPAS,
	DEBUG_RSS_SUB_UNMAP_MAX_BLOCK_PARTIAL,
	DEBUG_RSS_SUB_UNMAP_MAX_BLOCK,
	DEBUG_RSS_SUB_PUT_LOCAL_PAGE,
	DEBUG_RSS_SUB_SET_REMOTE,
	DEBUG_RSS_SUB_ALLOC_FETCH_ERR,
	DEBUG_RSS_SUB_COW_INACTIVE,
	DEBUG_RSS_SUB_COW_WRITEBACK,
	NUM_DEBUG_RSS_SUB_ID,
};

enum DEBUG_RSS_ADD_KERNEL_ID {
	DEBUG_RSS_ADD_KERNEL_VMA_OPEN,
	DEBUG_RSS_ADD_KERNEL_COW_MULTI_ACTIVE,
	NUM_DEBUG_RSS_ADD_KERNEL_ID,
};

enum DEBUG_RSS_SUB_KERNEL_ID {
	DEBUG_RSS_SUB_KERNEL_FREE_GPA_DIR,
	DEBUG_RSS_SUB_KERNEL_COW_MULTI_ACTIVE,
	NUM_DEBUG_RSS_SUB_KERNEL_ID,
};
#endif

struct work_request;

extern size_t initial_local_cache_pages; // counts in 4kb
#ifdef CONFIG_EMP_BLOCK
extern int    initial_block_order;
extern int    initial_subblock_order;
extern int    initial_use_compound_page;
extern int    initial_critical_subblock_first;
extern int    initial_critical_page_first;
extern int    initial_mark_empty_page;
extern int    initial_mem_poll;
extern int    initial_enable_transition_csf;
#endif
#ifdef CONFIG_EMP_OPT
extern int    initial_eval_media;
#endif
extern int    initial_remote_reuse;
extern int    initial_remote_policy_subblock;
extern int    initial_remote_policy_block;

struct emp_config {
#ifdef CONFIG_EMP_BLOCK
	u8      block_order;
	u8      subblock_order;
	int     critical_subblock_first;
	int     critical_page_first;
	int     enable_transition_csf;
	int     mark_empty_page;
	int     mem_poll;
	int     use_compound_page;
#endif
#ifdef CONFIG_EMP_STAT
	int     reset_after_read; /* reset statistics after reading them */
#endif
#ifdef CONFIG_EMP_OPT
	int     next_pt_premapping;
	int     eval_media;
#endif
	int     remote_reuse; /* reuse remote page: remote inclusive policy */
	int     remote_policy_subblock;
	int     remote_policy_block;
};

/* variables for collecting stats */
struct emp_stat {
	atomic_t read_reqs;
	atomic_t read_comp;
	atomic_t write_reqs;
	atomic_t write_comp;

	u64      recl_count;
	u64      post_read_count;
	u64      post_write_count;
	u64      stale_page_count;

	u64      remote_tlb_flush;
	u64      remote_tlb_flush_no_ipi;
	u64      remote_tlb_flush_force;

	u64      io_read_pages;
	u64      csf_fault;
	u64      csf_useful;
	u64      cpf_to_csf_transition;
	u64      post_read_mempoll;

	u64      fsync_count;
};

// second-tier memory for a mm
struct emp_mrs {
	atomic64_t       memregs_num_alloc; // how many times alloc_remote_page() called
	atomic_t         memregs_last_alloc; // mr->id of the last allocation
	u8               memregs_wdma_size;
	u32              memregs_block_size;
	// 256 memregs are supported for now
	u8               memregs_len;
	u8               memregs_last; // the last mrid added by 1
	struct memreg    **memregs;
	spinlock_t       memregs_lock;
#ifdef CONFIG_EMP_BLOCKDEV
	bool             blockdev_used;
#endif
	wait_queue_head_t mrs_ctrl_wq;
#ifdef CONFIG_EMP_USER
	struct kmem_cache *cow_remote_pages_cache;
#endif
};

#define GPADESC_MAX_REGION (6)
struct gpadesc_region {
	unsigned long start;
	unsigned long end;
	u8 block_order;
#ifdef CONFIG_EMP_VM
	bool lowmem_block;
#endif
#ifdef CONFIG_EMP_USER
	bool partial_map;
#endif
};

struct emp_vmdesc {
	atomic_t	        refcount;
	// length of vm descriptors
	unsigned long       	gpa_len;
	// directory of vm descriptors
	struct emp_gpa      	**gpa_dir;
	// granularity of vm descriptor and used to unit of DMA
	u8                  	subblock_order;
	// used to calculate hva for specific descriptor
	unsigned long       	vm_base; // subblock aligned vm_start address
	// correction for block-unaligned memory region
	// used to locate block's head
	unsigned long       	block_aligned_start;
	struct gpadesc_region	regions[GPADESC_MAX_REGION];
	int			num_region;
	// allocated virtually contiguous array from host
	void                	*gpa_dir_alloc;
	unsigned long           gpa_dir_alloc_size;
	spinlock_t		lock; // assure @refcount stable
};

// virtual memory region for a contiguous host virtual (mmaped) memory
struct emp_vmr {
	unsigned int        magic; // magic value
	int                 id;
	int                 pvid;
	struct emp_mm       *emm;
	struct vm_area_struct *host_vma;
	struct mm_struct      *host_mm;
	atomic_long_t       rss_cache;
#ifdef CONFIG_EMP_DEBUG_RSS
	atomic_long_t       debug_rss_add_total;
	atomic_long_t       debug_rss_sub_total;
	atomic_long_t       debug_rss_add[NUM_DEBUG_RSS_ADD_ID];
	atomic_long_t       debug_rss_sub[NUM_DEBUG_RSS_SUB_ID];
	atomic_long_t       debug_rss_add_kernel_total;
	atomic_long_t       debug_rss_sub_kernel_total;
	atomic_long_t       debug_rss_add_kernel[NUM_DEBUG_RSS_ADD_KERNEL_ID];
	atomic_long_t       debug_rss_sub_kernel[NUM_DEBUG_RSS_SUB_KERNEL_ID];
#endif

	/* descriptor (struct emp_gpa) array for host_vma */
	struct emp_vmdesc   *descs;
	struct emp_gpa *(*new_gpadesc)(struct emp_vmr *vmr, unsigned long idx);
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	void (*set_gpadesc_alloc_at)(struct emp_vmr *, unsigned long, char *, int);
#endif

	int                 vmr_closing;

	spinlock_t          gpas_close_lock;
	/* for unmapping on gpas_close() */
	struct mmu_gather   close_tlb;

#ifdef CONFIG_EMP_USER
	unsigned long       split_addr;
	struct emp_vmr      *new_vmr;

	struct emp_mmu_notifier *mmu_notifier;

	/* The following variables are protected by vmr->emm->dup_list_lock */
	struct list_head        dup_shared;   /* MAP_SHARED */
	struct emp_vmr		*dup_parent;  /* MAP_PRIVATE, parent */
	struct list_head	dup_children; /* MAP_PRIVATE, children */
	struct list_head	dup_sibling;  /* MAP_PRIVATE, sibling */
#endif
};

struct emp_ftm {
	atomic_t            local_cache_pages;
	int                 local_cache_pages_headroom;
	atomic_t            alloc_pages_len;

	/* list of free pages which is maintained globally */
	struct emp_list     free_page_list;
	wait_queue_head_t   free_pages_wq; //wait queue
	atomic_t            free_pages_reclaim;
	int                 per_vcpu_free_lpages_len;

	/* lists of pages maintained to use local DRAM efficiently */
	struct slru         active_list;
	size_t              active_pages_len;
	struct slru         inactive_list;
	size_t              inactive_pages_len;

	struct kmem_cache   *local_pages_cache;
	struct kmem_cache   *mapped_pmd_cache;
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	struct list_head local_page_list;
	spinlock_t local_page_list_lock;
#endif
};

// ops to control data copy between first tier and second tier
struct emp_stm_ops {
	/* function pointers for page management */
	int (*alloc_and_fetch_pages)(struct emp_vmr *, struct emp_gpa *,
				unsigned long, int, int, struct vcpu_var *,
				struct work_request *, struct work_request **,
				int, bool, bool, bool);
	struct work_request *
		(*post_writeback_async)(struct emp_mm *, struct emp_gpa *,
				struct vcpu_var *,
				struct work_request *, struct work_request *,
				int, bool);
	void (*clear_writeback_block)(struct emp_mm *, struct emp_gpa *,
				      struct work_request *, struct vcpu_var *,
				      bool, bool);
	int (*push_writeback_request)(struct emp_mm *, struct work_request *,
				      struct vcpu_var *);
	int (*wait_writeback_async)(struct emp_mm *, struct vcpu_var *, bool);
	int (*wait_writeback_async_steal)(struct emp_mm *, struct vcpu_var *); 
	bool (*wait_read_async)(struct emp_mm *, struct vcpu_var *,
				struct emp_gpa *);
	bool (*try_wait_read_async)(struct emp_mm *, struct vcpu_var *,
				    struct emp_gpa *);
	int (*wait_read_async_demand_page)(struct emp_mm *, struct vcpu_var *,
					    struct emp_gpa *, unsigned int);
};

// ops to allocate/deallocate descriptor for local page
struct emp_lps_ops {
	/* function pointers for local page management */
	struct local_page *(*alloc_local_page)(struct emp_mm *, int,
			struct memreg *, struct page *, int, off_t, 
			struct emp_gpa *);
	void (*free_local_page)(struct emp_mm *, struct local_page *);
};

// ops to maintaine host virtual address
struct emp_vm_ops {
	/* function pointers for gpa management */
	void (*unmap_gpas)(struct emp_mm *, struct emp_gpa *, bool *);
	void (*free_gpa)(struct emp_mm *, struct emp_gpa *,
						struct vcpu_var *);
	void (*set_gpa_remote)(struct emp_mm *, struct vcpu_var *,
			       struct emp_gpa *);
};

// ops to create/distruct a memory region
struct emp_mr_ops {
	/* function pointers for memory region management */
	int (*create_mr)(struct emp_mm *, struct donor_info *, int *);
	void (*disconnect_mr)(struct memreg *);
};

#ifdef CONFIG_EMP_USER
// ops to manage Copy-on-Write memory regions.
struct emp_cow_ops {
	/* function pointer for Copy-on-Write memory region management */
	int (*handle_emp_cow_fault_hva)(struct emp_mm *, struct emp_vmr *,
					struct emp_gpa *, unsigned long,
					struct vm_fault *);
};
#endif

#ifdef CONFIG_EMP_VM
struct emp_exp_kvm {
	struct kvm  *kvm;
	int         kvm_vcpus_len; // number of virtual cpus of KVM
	int         emp_vcpus_len; // number of virtual cpus of KVM + io threads for EMP
	unsigned long apic_base_hva;
	int         memslot_len;
	struct {
		unsigned long hva;
		unsigned long gpa;
		unsigned long size;
		unsigned long base;
	} memslot[2];
};
#endif

struct emp_gpadesc_alloc {
	struct kmem_cache *alloc[BLOCK_MAX_ORDER + 1];
	spinlock_t lock;
};

#define gpadesc_alloc_lock(emm) spin_lock(&(emm)->gpadesc_alloc.lock)
#define gpadesc_alloc_unlock(emm) spin_unlock(&(emm)->gpadesc_alloc.lock)
#define __get_gpadesc_alloc(emm, order) ((emm)->gpadesc_alloc.alloc[order])
#define __set_gpadesc_alloc(emm, order, cachep) do { \
	(emm)->gpadesc_alloc.alloc[order] = (cachep); \
} while (0)
#ifdef CONFIG_EMP_DEBUG
#define get_gpadesc_alloc(emm, order) ({ \
	BUG_ON(!(emm)); \
	BUG_ON(!(emm)->gpadesc_alloc.alloc[order]); \
	__get_gpadesc_alloc(emm, order); \
})

#define set_gpadesc_alloc(emm, order, cachep) do { \
	BUG_ON(!(emm)); \
	BUG_ON((emm)->gpadesc_alloc.alloc[order]); \
	__set_gpadesc_alloc(emm, order, cachep); \
} while (0)
#else
#define get_gpadesc_alloc(emm, order) __get_gpadesc_alloc(emm, order)
#define set_gpadesc_alloc(emm, order, val) __set_gpadesc_alloc(emm, order, val);
#endif

/* return non-NULL if race condition is detected. */
#define set_gpa_dir_new(vmr, gpa_dir, idx, new) ({ \
	struct emp_gpa *____g = (struct emp_gpa *) atomic64_cmpxchg( \
						(atomic64_t *) &((gpa_dir)[idx]), \
						(s64) NULL, (s64) (new)); \
	if (likely(____g == NULL)) { \
		atomic_inc(&(new)->refcnt); \
		debug_gpa_refcnt_inc_mark((new), (vmr)->id); \
	} else \
		dprintk_ratelimited(KERN_ERR "%s: race at set_gpa_dir_new() " \
				"is detected. vmr: %d index: 0x%lx " \
				"new: 0x%016lx prev: 0x%016lx\n", \
				__func__, (vmr)->id, (idx), \
				(unsigned long) (new), (unsigned long) ____g); \
	____g; \
})

#ifdef CONFIG_EMP_DEBUG
#define change_gpa_dir(vmr, gpa_dir, idx, old, new) do { \
	struct emp_gpa *____prev, *____old_head; \
	____old_head = emp_get_block_head(old); \
	debug_assert(____emp_gpa_is_locked(____old_head)); \
	____prev = (struct emp_gpa *) atomic64_cmpxchg( \
					(atomic64_t *) &((gpa_dir)[idx]), \
					(s64) (old), (s64) (new)); \
	if (unlikely(____prev != (old))) \
		printk_ratelimited(KERN_ERR "%s: race at change_gpa_dir() " \
				"is detected. vmr: %d index: 0x%lx " \
				"old: 0x%016lx new: 0x%016lx prev: 0x%016lx\n", \
				__func__, (vmr)->id, (idx), \
				(unsigned long) old, (unsigned long) new, \
				(unsigned long) ____prev); \
	atomic_inc(&(new)->refcnt); \
	debug_gpa_refcnt_inc_mark((new), (vmr)->id); \
	BUG_ON(atomic_dec_return(&(old)->refcnt) <= 0); \
	debug_gpa_refcnt_dec_mark((old), (vmr)->id); \
} while (0)

#else
#define change_gpa_dir(vmr, gpa_dir, idx, old, new) do { \
	(gpa_dir)[idx] = (new); \
	atomic_inc(&(new)->refcnt); \
	debug_gpa_refcnt_inc_mark((new), (vmr)->id); \
	atomic_dec(&(old)->refcnt); \
	debug_gpa_refcnt_dec_mark((old), (vmr)->id); \
} while (0)
#endif

#define remove_gpa_dir(vmr, gpa_dir, idx) do { \
	struct emp_gpa *____gpa = (gpa_dir)[idx]; \
	(gpa_dir)[idx] = NULL; \
	atomic_dec(&____gpa->refcnt); \
	debug_gpa_refcnt_dec_mark(____gpa, (vmr)->id); \
} while (0)

#ifdef CONFIG_EMP_USER
struct emp_mmu_notifier {
	struct mmu_notifier mmu_notifier;
	struct emp_mm *emm;
#ifdef CONFIG_EMP_DEBUG
	atomic_t refcnt;
#endif
};
#endif

struct emp_mm {
	atomic_t            refcount;
	int                 id;
	pid_t               pid;
	int                 possible_cpus;
	struct mm_struct    *last_mm;

#ifdef CONFIG_EMP_VM
	struct emp_exp_kvm  ekvm;
#endif
	struct srcu_struct  srcu;
	int                 close;

	struct proc_dir_entry *emp_proc_dir;
#ifdef CONFIG_EMP_STAT
	struct proc_dir_entry *emp_stat_dir;
#endif
	struct emp_config   config;

	DECLARE_BITMAP(vmrs_bitmap, EMP_VMRS_MAX);
	int                 vmrs_len;
	struct emp_vmr      **vmrs;
	struct emp_vmr      *last_vmr;
	struct emp_gpadesc_alloc gpadesc_alloc;

	struct emp_ftm      ftm;
	struct emp_mrs      mrs;

#ifdef CONFIG_EMP_VM
	struct vcpu_var     *vcpus;
#endif
	struct vcpu_var __percpu *pcpus;

#ifdef CONFIG_EMP_STAT
	/* variables for collecting stats */
	struct emp_stat     stat;
#endif

	struct emp_stm_ops  sops;
	struct emp_lps_ops  lops;
	struct emp_vm_ops   vops;
	struct emp_mr_ops   mops;
#ifdef CONFIG_EMP_USER
	struct emp_cow_ops  cops;

	/* protect dup_* variables of struct emp_vmr */
	spinlock_t         dup_list_lock;
#endif
};

#ifdef CONFIG_EMP_BLOCK
#define bvma_block_order(b)     ((b)->config.block_order)
#define bvma_block_size(b)      (1 << ((b)->config.block_order))
#define bvma_subblock_order(b)  ((b)->config.subblock_order)
#define bvma_subblock_size(b)   (1 << ((b)->config.subblock_order))
#define bvma_subblock_mask(b)   (bvma_subblock_size(b) - 1)
#define bvma_csf_enabled(b)     ((b)->config.critical_subblock_first)
#define bvma_cpf_enabled(b)     ((b)->config.critical_page_first)
#define bvma_transition_csf(b)  ((b)->config.enable_transition_csf)
#define bvma_mark_empty_page(b) ((b)->config.mark_empty_page)
#define bvma_mem_poll(b)        ((b)->config.mem_poll)
#else
#define bvma_block_order(b)     (0)
#define bvma_block_size(b)      (1)
#define bvma_subblock_order(b)  (0)
#define bvma_subblock_size(b)   (1)
#define bvma_subblock_mask(b)   (0)
#define bvma_csf_enabled(b)     (0)
#define bvma_cpf_enabled(b)     (0)
#define bvma_transition_csf(b)  (0)
#define bvma_mark_empty_page(b) (0)
#define bvma_mem_poll(b)        (0)
#endif

#define bvma_sib_order(b)       (bvma_block_order(b) - bvma_subblock_order(b))
#define bvma_sib_size(b)        (1 << bvma_sib_order(b))
#define bvma_sib_mask(b)        (bvma_sib_size(b) - 1)

#define bvma_va_block_order(b)    (bvma_block_order(b) + PAGE_SHIFT)
#define bvma_va_block_size(b)     (1 << bvma_va_block_order(b))
#define bvma_va_block_mask(b)     (bvma_va_block_size(b) - 1)

#define bvma_va_subblock_order(b) (bvma_subblock_order(b) + PAGE_SHIFT)
#define bvma_va_subblock_size(b)  (1 << bvma_va_subblock_order(b))
#define bvma_va_subblock_mask(b)  (bvma_va_subblock_size(b) - 1)

#define bvma_va_sib_order(b)      (bvma_sib_order(b) + PAGE_SHIFT)
#define bvma_va_sib_size(b)       (1 << bvma_va_sib_order(b))
#define bvma_va_sib_mask(b)       (bvma_va_sib_size(b) - 1)

#define vmdesc_subblock_order(d)  (d->subblock_order)
#define vmdesc_subblock_size(d)   (1 << vmdesc_subblock_order(d))
#define vmdesc_subblock_mask(d)   (vmdesc_subblock_size(d) - 1)

#ifdef CONFIG_EMP_BLOCK
#define EMPTY_PAGE	((u64)(0xDEADBEEF)) // for CPF
#endif

#define VCPU_START_ID (1)

#define IO_THREAD_MAX   ((bvma)->possible_cpus)
#define IO_THREAD_ID    (0)
#ifdef CONFIG_EMP_VM
#define KVM_THREAD_MAX	((bvma)->ekvm.emp_vcpus_len - 1)
#else
#define KVM_THREAD_MAX	(0)
#endif
#ifdef CONFIG_EMP_VM
#ifdef CONFIG_EMP_USER
#define EMP_MAIN_CPU_LEN(bvma) ((bvma)->ekvm.emp_vcpus_len > 0 \
				? (bvma)->ekvm.emp_vcpus_len \
				: num_possible_cpus())
#else /* !CONFIG_EMP_USER */
#define EMP_MAIN_CPU_LEN(bvma) ((bvma)->ekvm.emp_vcpus_len)
#endif /* !CONFIG_EMP_USER */
#else /* !CONFIG_EMP_VM */
#define EMP_MAIN_CPU_LEN(bvma) num_possible_cpus()
#endif /* !CONFIG_EMP_VM */
#ifdef CONFIG_EMP_VM
#define EMP_KVM_VCPU_LEN(bvma) ((bvma)->ekvm.emp_vcpus_len)
#else
#define EMP_KVM_VCPU_LEN(bvma) (0)
#endif
#define IS_IOTHREAD_VCPU(c) ((c) < IO_THREAD_ID)

/* (note) Definition of VCPU, IO_THREAD, KVM_THREAD in EMP
 * 	VCPU = IO_THREAD + KVM_THREAD
 * 	In user-level EMP, KVM_THREAD does not exist
 * 	the number of IO_THREAD is the same as the number of physical CPUs
 * 	cpu_id 0 is not used, then the range of cpu_id = [-#IO_THREAD ~ -1], [0 ~ #KVM_THREAD]
 */
#define FOR_EACH_VCPU(bvma, i) \
	for((i) = -IO_THREAD_MAX; (i) <= KVM_THREAD_MAX; ((i) == -1? (i) += 2: (i)++))

#define FOR_EACH_IOTHREAD(bvma, i) \
	for((i) = -IO_THREAD_MAX; (i) < 0; (i)++)

#define FOR_EACH_KVM_THREAD(bvma, i) \
	for((i) = VCPU_START_ID; (i) <= KVM_THREAD_MAX; (i)++)

#ifdef CONFIG_EMP_VM
#define FOR_EACH_MEMSLOT(bvma, index) \
	for(index = 0; index < bvma->ekvm.memslot_len; index++)
#endif

#ifdef CONFIG_EMP_VM
#define is_emm_with_kvm(emm) ((emm)->ekvm.kvm ? true : false)
#else /* !CONFIG_EMP_VM */
#define is_emm_with_kvm(emm) (false)
#endif /* !CONFIG_EMP_VM */

struct emp_mm **get_emp_mm_arr(void);

static inline u64 get_ts_in_ns(void)
{
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 6, 0))
	struct timespec ts;
	getnstimeofday(&ts);
#else
	struct timespec64 ts;
	ktime_get_real_ts64(&ts);
#endif
	return (S_TO_NS * ts.tv_sec) + ts.tv_nsec;
}
		
// naive lookup function
static inline struct emp_vmr *
emp_vmr_lookup(struct emp_mm *emm, struct vm_area_struct *vma)
{
	int p, count;
	if (emm->last_vmr && (emm->last_vmr->host_vma == vma))
		return emm->last_vmr;

	p = 0;
	count = 0;
	for_each_clear_bit_from(p, emm->vmrs_bitmap, EMP_VMRS_MAX) {
		if (++count >= emm->vmrs_len)
			break;
		if (emm->vmrs[p]->host_vma == vma) {
			emm->last_vmr = emm->vmrs[p];
			return emm->vmrs[p];
		}
	}

	return NULL;
}

#define VA_IN_VMA(vma, va) \
	!((va < vma->vm_start) || (hva >= vma->vm_end))
static inline struct emp_vmr *
emp_vmr_lookup_hva(struct emp_mm *emm, const unsigned long hva)
{
	int p, count;
	if (emm->last_vmr && (VA_IN_VMA(emm->last_vmr->host_vma, hva)))
		return emm->last_vmr;

	p = 0;
	count = 0;
	for_each_clear_bit_from(p, emm->vmrs_bitmap, EMP_VMRS_MAX) {
		if (count++ >= emm->vmrs_len)
			break;
		if (VA_IN_VMA(emm->vmrs[p]->host_vma, hva)) {
			emm->last_vmr = emm->vmrs[p];
			return emm->vmrs[p];
		}
	}

	return NULL;
}

static inline void
__emp_update_rss_add(struct emp_vmr *vmr, unsigned long val)
{
	atomic_long_add(val, &vmr->rss_cache);
}

static inline void
__emp_update_rss_sub(struct emp_vmr *vmr, unsigned long val)
{
	atomic_long_sub(val, &vmr->rss_cache);
}

static inline void
__emp_update_rss_add_force(struct emp_vmr *vmr, unsigned long val)
{
	emp_add_mm_counter(vmr->host_mm, MM_FILEPAGES, val);
}

static inline void
__emp_update_rss_sub_force(struct emp_vmr *vmr, unsigned long val)
{
	emp_add_mm_counter(vmr->host_mm, MM_FILEPAGES, -val);
}

static inline void emp_update_rss_cached(struct emp_vmr *vmr)
{
	long val = atomic_long_xchg(&vmr->rss_cache, 0);
	if (likely(val != 0))
		emp_add_mm_counter(vmr->host_mm, MM_FILEPAGES, val);
}

static inline unsigned int emp_smp_processor_id(void)
{
	unsigned int ret;
	preempt_disable();
	ret = smp_processor_id();
	preempt_enable();
	return ret;
}

#ifdef CONFIG_EMP_DEBUG_RSS
#define DEBUG_UPDATE_RSS_BLOCK 0
#define DEBUG_UPDATE_RSS_SUBBLOCK 1
#define emp_update_rss_add(vmr, val, ID, gpa, mode) do { \
	debug_update_rss_add(vmr, val, ID, 1, gpa, mode, __FILE__, __LINE__); \
	__emp_update_rss_add(vmr, val); \
} while (0)
#define emp_update_rss_sub(vmr, val, ID, gpa, mode) do { \
	debug_update_rss_sub(vmr, val, ID, 1, gpa, mode, __FILE__, __LINE__); \
	__emp_update_rss_sub(vmr, val); \
} while (0)
#define emp_update_rss_add_force(vmr, val, ID, gpa, mode) do { \
	debug_update_rss_add(vmr, val, ID, 1, gpa, mode, __FILE__, __LINE__); \
	__emp_update_rss_add_force(vmr, val); \
} while (0)
#define emp_update_rss_sub_force(vmr, val, ID, gpa, mode) do { \
	debug_update_rss_sub(vmr, val, ID, 1, gpa, mode, __FILE__, __LINE__); \
	__emp_update_rss_sub_force(vmr, val); \
} while (0)
#else /* !CONFIG_EMP_DEBUG_RSS */
#define emp_update_rss_add(vmr, val, ID, gpa, mode) __emp_update_rss_add(vmr, val)
#define emp_update_rss_sub(vmr, val, ID, gpa, mode) __emp_update_rss_sub(vmr, val)
#define emp_update_rss_add_force(vmr, val, ID, gpa, mode) __emp_update_rss_add_force(vmr, val)
#define emp_update_rss_sub_force(vmr, val, ID, gpa, mode) __emp_update_rss_sub_force(vmr, val)
#endif

#ifdef CONFIG_EMP_DEBUG_RSS
#define emp_update_rss_add_kernel(vmr, val, ID, gpa, mode) do { \
	debug_update_rss_add(vmr, val, ID, 0, gpa, mode, __FILE__, __LINE__); \
} while (0)

#define emp_update_rss_sub_kernel(vmr, val, ID, gpa, mode) do { \
	debug_update_rss_sub(vmr, val, ID, 0, gpa, mode, __FILE__, __LINE__); \
} while (0)

void __emp_update_rss_show(struct emp_vmr *vmr, const char *func);
#define emp_update_rss_show(vmr) __emp_update_rss_show(vmr, __func__)
#else
#define emp_update_rss_add_kernel(vmr, val, ID, gpa, mode) do {} while (0)
#define emp_update_rss_sub_kernel(vmr, val, ID, gpa, mode) do {} while (0)
#define emp_update_rss_show(vmr) do {} while (0)
#endif


#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0))
// From RHEL 9, vmf->pgoff is a constant variable.
// But sometimes we need to modify it.
#define set_vmf_pgoff(vmf_ptr, new_pgoff) do { \
	*((pgoff_t *)&(vmf_ptr)->pgoff) = (new_pgoff); \
} while (0)
#define set_vmf_address(vmf_ptr, new_address) do { \
	*((unsigned long *)&(vmf_ptr)->address) = (new_address); \
} while (0)
#define add_vmf_pgoff(vmf_ptr, val) do { \
	*((pgoff_t *)&(vmf_ptr)->pgoff) += (val); \
} while (0)
#define add_vmf_address(vmf_ptr, val) do { \
	*((unsigned long *)&(vmf_ptr)->address) += (val); \
} while (0)
#else
#define set_vmf_pgoff(vmf_ptr, new_pgoff) do { \
	(vmf_ptr)->pgoff = (new_pgoff); \
} while (0)
#define set_vmf_address(vmf_ptr, new_address) do { \
	(vmf_ptr)->address = (new_address); \
} while (0)
#define add_vmf_pgoff(vmf_ptr, val) do { \
	(vmf_ptr)->pgoff += (val); \
} while (0)
#define add_vmf_address(vmf_ptr, val) do { \
	(vmf_ptr)->address += (val); \
} while (0)
#endif

#endif /* __EMP_H__ */
