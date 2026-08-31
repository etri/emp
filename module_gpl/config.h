#ifndef __CONFIG_H__
#define __CONFIG_H__

#define CONFIG_EMP_SHOW_FAULT_PROGRESS // show page fault address at every CONFIG_EMP_SHOW_FAULT_PROGRESS_PERIOD faults

/* Instrumentation for the lazy fork: prints one summary per fork covering which
 * block states a child inherits through the metadata-only duplication, and
 * whether write-protecting the parent raced EMP's reclaim. Deliberately outside
 * CONFIG_EMP_DEBUG so it can be switched on in a production-config build (the
 * CONFIG_EMP_DEBUG_RSS toggles below sit inside that guard and are inert there).
 * Enabled here for validation; comment it out before merging. */
#define CONFIG_EMP_DEBUG_LAZY_FORK

#ifdef CONFIG_EMP_DEBUG
//#define CONFIG_EMP_DEBUG_PROGRESS
//#define CONFIG_EMP_DEBUG_PROGRESS_GPA_LOCK
//#define CONFIG_EMP_DEBUG_GPA_STATE
//#define CONFIG_EMP_DEBUG_INTEGRITY
//#define CONFIG_EMP_DEBUG_ALLOC
//#define CONFIG_EMP_DEBUG_GPADESC_ALLOC
#define CONFIG_EMP_DEBUG_SHOW_GPA_STATE 128 // show gpa state only when the number of gpa descriptors of a VMR is smaller or equal to this number
//#define CONFIG_EMP_DEBUG_SHOW_PROGRESS // show the progress of long running functions
//#define CONFIG_EMP_DEBUG_LRU_LIST
//#define CONFIG_EMP_DEBUG_LRU_LIST_DEL
//#define CONFIG_EMP_DEBUG_PAGE_REF
//#define CONFIG_EMP_DEBUG_PAGE_REF_LESS_ONLY // do not treat correct > page_count(p) as an error.
//#define CONFIG_EMP_DEBUG_GPA_REFCNT
//#define CONFIG_EMP_DEBUG_RSS
//#define CONFIG_EMP_DEBUG_RSS_PROGRESS
#define CONFIG_EMP_DEBUG_RSS_MAX_VMRS (128) // maximum number of vmr supported by RSS debugger
//#define CONFIG_EMP_DEBUG_SYNC_HPT // check that every subblock of a block has the same mapper set
//#define CONFIG_EMP_DEBUG_PF_HISTORY
//#define CONFIG_EMP_DEBUG_TRIGGER_REDUCE // Skip setting referenced flag on gpa at exit. More reduce operations will occur.
#endif

#define COMPILER_OPT __attribute__((optimize("-O2")))
#define COMPILER_DEBUG_FORCE __attribute__((optimize("-O0")))
#define EMP_DEVICE_NAME "emp"
#define EMP_MM_MAX      (256)

#if defined(CONFIG_EMP_DEBUG_GPA_STATE) || defined(COFIG_EMP_DEBUG_PROGRESS_LOCK)
/* DEBUG_GPA_STATE uses DEBUG_PROGRESS */
#ifndef CONFIG_EMP_DEBUG_PROGRESS
#define CONFIG_EMP_DEBUG_PROGRESS
#endif
#endif

/*
 * default max virtual memory areas vm.max_map_count: 65530
 * $ sysctl vm.max_map_count
 * vm.max_map_count = 65530
 *
 * temporary change: ex)$ sysctl -w vm.max_map_count=262144
 * permanent change: ex)$ vi /etc/sysctl.conf
 *                      $ sysctl -p
 */
#ifdef CONFIG_EMP_DEBUG_SYNC_HPT
/* Where a mapper-set check was made. The names describe the moment, not the
 * code: the point of the checker is to say which of them the current
 * synchronization policy actually needs. */
enum debug_sync_hpt_point {
	DEBUG_SYNC_HPT_AFTER_SYNC,	/* just after sync_hpt_map_in_block() */
	DEBUG_SYNC_HPT_AFTER_INSTALL,	/* after the faulting vmr's installation */
	DEBUG_SYNC_HPT_AT_UNMAP,	/* at the unmap-time synchronization */
	NUM_DEBUG_SYNC_HPT_POINTS,
};
#endif


#ifdef CONFIG_EMP_DEBUG
/* Size of the debug-only vmr id space. Ids are reused as vmrs come and go, so
 * they stay small and stay useful as indices for the rss debugger, which keeps
 * a per-vmr bit per local page and gives up above
 * CONFIG_EMP_DEBUG_RSS_MAX_VMRS. A debug id has no functional meaning: nothing
 * is looked up by it. */
#define EMP_DEBUG_VMR_IDS_MAX (1024)
#endif

#ifdef CONFIG_EMP_VM
/* NOTE: LOW_MEMORY_MAX_ORDER
 * LOW_MEMORY_MAX_ORDER is the maximum block order of the first 2MB region in
 * virtuam machines. It was 7 (512KB). However, experiments in 20, Oct, 2023
 * show that LOW_MEMORY_MAX_ORDER should be 3 (32KB). Honestly, we don't know
 * why. However, if we set the block size of a VM larger than 32KB and boot up
 * a VM and immediately powered off, the host system gets a kernel BUG at
 * kvm_mmu_zap_all(). The otensible reason of BUG() is that some pages in the
 * low memory region has a present shadow page table entry, but does not have
 * its reverse mapping. We need to check the things in the future.
 *
 * In 12, Nov, 2023, we decide to keep the low memory region on local cache.
 * LOW_MEMORY_MAX_ORDER is set as 9 (2MB) to be covered by a single block.
 * 1. emp_page_fault_gpa() lets KVM handle the low memory region.
 * 2. emp_page_fault_hva() directly handles the low memory region since it does
 *    not have any fall-back mechanism. Instead, we keep it on local cache by
 *    checking GPA_LOWMEM_BLOCK_MASK at gpa_acquire(). gpa_acquire() never
 *    returns true for low memory region gpas.
 */
#define LOW_MEMORY_REGION_SIZE (2 << 20) // 2MB
#define LOW_MEMORY_MAX_ORDER (9) // 2MB
#endif // CONFIG_EMP_VM

#ifdef CONFIG_EMP_BLOCK
#define BLOCK_MAX_ORDER (9)
#define BLOCK_MAX_SIZE  (1 << BLOCK_MAX_ORDER)
#else
#define BLOCK_MAX_ORDER (0)
#define BLOCK_MAX_SIZE  (1)
#endif
#define DMA_OPERATION_MAX_ORDER (5) // maximum size of dma operation is 128KB
#define DMA_WRITEBACK_MAX_ORDER (5U)

#define NUM_VICTIM_CLUSTER (8)

// high PER_VCPU_WB_REQUESTS_MAX increases latency of wb page handling
#define PER_VCPU_WB_REQUESTS_MAX(b) (128 << bvma_subblock_order(b))
#ifdef CONFIG_EMP_BLOCK
#define PER_VCPU_FREE_LPAGES_LEN(emm, cnt) \
	((1 << ((emm)->config.block_order - (emm)->config.subblock_order)) * (cnt))
#else
#define PER_VCPU_FREE_LPAGES_LEN(emm, cnt) (cnt)
#endif

#ifdef CONFIG_EMP_RDMA
#define RDMA_TIMEOUT_MS (2000)
#define RDMA_CONN_RETRY_COUNT (10)
#define DONOR_PORT            (19675)
#endif

#ifdef CONFIG_EMP_OPT
#define DEFAULT_CHAINED_OPERATION (1)
#endif

#define LOCAL_CACHE_SIZE      (4 * SIZE_GIGA)
#define LOCAL_CACHE_PAGES     (LOCAL_CACHE_SIZE >> PAGE_SHIFT)

#define LOWER_MEM_SIZE        (256 * SIZE_MEGA)

#define DEFAULT_REMOTE_REUSE (1)

enum {
	REMOTE_POLICY_SUBBLOCK_ANY,
	REMOTE_POLICY_SUBBLOCK_SAME_MR, // required to chained operation
	REMOTE_POLICY_SUBBLOCK_DIFF_MR,
	NUM_REMOTE_POLICY_SUBBLOCK
};
#define DEFAULT_REMOTE_POLICY_SUBBLOCK REMOTE_POLICY_SUBBLOCK_DIFF_MR

#define REMOTE_POLICY_BLOCK_DISTRIBUTE_MR (1)
#define REMOTE_POLICY_BLOCK_SEQUENTIAL_MR (0) // for SSD devices
#define DEFAULT_REMOTE_POLICY_BLOCK REMOTE_POLICY_BLOCK_DISTRIBUTE_MR

#endif /* __CONFIG_H__ */
