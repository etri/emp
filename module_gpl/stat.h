#ifndef __STAT_H__
#define __STAT_H__
#ifdef CONFIG_EMP_STAT
/* macros for global statistics */
#define emp_stat_inc(emm, name) do { \
		atomic64_inc(&(emm)->stat.name); \
} while (0)
#define emp_stat_add(emm, name, val) do { \
		atomic64_add((val), &(emm)->stat.name); \
} while (0)
#define emp_stat_read(emm, name) ({ \
		atomic64_read(&(emm)->stat.name); \
})
#define emp_stat_reset(emm, name) do { \
		atomic64_set(&(emm)->stat.name, 0); \
} while (0)
#define emp_stat_init(emm) do { \
		memset(&(emm)->stat, 0, sizeof(struct emp_stat)); \
} while (0)

/* macros for per-cpu statistics */
#define emp_vcpu_stat_inc(cpu, name) do { \
		(cpu)->stat.name += 1; \
} while (0)
#define emp_vcpu_stat_add(cpu, name, val) do { \
		(cpu)->stat.name += (val); \
} while (0)
#define emp_vcpu_stat_read(cpu, name) ({ \
		(cpu)->stat.name; \
})
#define emp_vcpu_stat_reset(cpu, name) do { \
		(cpu)->stat.name = 0; \
} while (0)
#define emp_vcpu_stat_init(cpu) do { \
		memset(&(cpu)->stat, 0, sizeof(struct emp_vcpu_stat)); \
} while (0)

#ifdef CONFIG_EMP_ELASTIC_BLOCK
/* macros for elastic block statistics */
#define emp_els_stat_inc(emm, gpa, name) do { \
	atomic64_inc(&(emm)->stat.els_##name[gpa_block_order(gpa)]); \
} while (0)
#define emp_els_stat_add(emm, gpa, name, val) do { \
	atomic64_add((val), &(emm)->stat.els_##name[gpa_block_order(gpa)]); \
} while (0)
#define __emp_els_stat_add(emm, order, name, val) do { \
	atomic64_add((val), &(emm)->stat.els_##name[order]); \
} while (0)
#define emp_els_stat_read(emm, name, order) ({ \
	atomic64_read(&(emm)->stat.els_##name[order]); \
})
#define emp_els_stat_reset(emm, name, order) do { \
	atomic64_set(&(emm)->stat.els_##name[order], 0); \
} while (0)
#else /* !CONFIG_EMP_ELASTIC_BLOCK */
#define emp_els_stat_inc(emm, gpa, name) do {} while (0)
#define emp_els_stat_add(emm, gpa, name, val) do {} while (0)
#define __emp_els_stat_add(emm, order, name, val) do {} while (0)
#define emp_els_stat_read(emm, name, order) (0)
#define emp_els_stat_reset(emm, name, order) do {} while (0)
#endif /* !CONFIG_EMP_ELASTIC_BLOCK */

/* variables for collecting stats */
struct emp_stat {
	atomic64_t read_reqs;
	atomic64_t read_comp;
	atomic64_t write_reqs;
	atomic64_t write_comp;

	atomic64_t      recl_count;
	atomic64_t      post_read_count;
	atomic64_t      post_write_count;
	atomic64_t      stale_page_count;

	atomic64_t      remote_tlb_flush;
	atomic64_t      remote_tlb_flush_no_ipi;
	atomic64_t      remote_tlb_flush_force;

	atomic64_t      io_read_pages;
	atomic64_t      csf_fault;
	atomic64_t      csf_useful;
	atomic64_t      cpf_to_csf_transition;
	atomic64_t      post_read_mempoll;

	atomic64_t      fsync_count;

#ifdef CONFIG_EMP_ELASTIC_BLOCK
	atomic64_t els_block_count[BLOCK_MAX_ORDER + 1];
	atomic64_t els_block_stretch[BLOCK_MAX_ORDER + 1];
	atomic64_t els_block_reduce[BLOCK_MAX_ORDER + 1];
	atomic64_t els_block_reduce_complete[BLOCK_MAX_ORDER + 1];
	atomic64_t els_fetch[BLOCK_MAX_ORDER + 1];
	atomic64_t els_writeback[BLOCK_MAX_ORDER + 1];
	atomic64_t els_noref_count[BLOCK_MAX_ORDER + 1];
	atomic64_t els_ref_count[BLOCK_MAX_ORDER + 1];
	atomic64_t els_clean_count[BLOCK_MAX_ORDER + 1];
	atomic64_t els_dirty_count[BLOCK_MAX_ORDER + 1];
	atomic64_t els_complete_noref_count[BLOCK_MAX_ORDER + 1];
	atomic64_t els_complete_clean_count[BLOCK_MAX_ORDER + 1];
#endif

	atomic64_t      blk_prefetch_try;
	atomic64_t      blk_prefetch_active;
	atomic64_t      blk_prefetch_inactive;
	atomic64_t      blk_prefetch_writeback;
	atomic64_t      blk_prefetch_remote;
	atomic64_t      blk_dontneed_try;
	atomic64_t      blk_dontneed_succeed;
	atomic64_t      blk_dontneed_active;
	atomic64_t      blk_dontneed_inactive;
	atomic64_t      blk_dontneed_writeback;
	atomic64_t      blk_dontneed_remote;
};

struct emp_vcpu_stat {
	u64 vma_fault;
	u64 local_fault;
	u64 remote_fault;
	u64 dbit_count;
	u64 cbit_count;
	u64 wb_count;
	u64 alloc_pages_wait_count;
};
#else /* !CONFIG_EMP_STAT */
#define emp_stat_inc(emm, name) do {} while (0)
#define emp_stat_add(emm, name, val) do {} while (0)
#define emp_stat_read(emm, name) (0)
#define emp_stat_reset(emm, name) do {} while (0)
#define emp_stat_init(emm) do {} while (0)

#define emp_vcpu_stat_inc(cpu, name) do {} while (0)
#define emp_vcpu_stat_add(cpu, name, val) do {} while (0)
#define emp_vcpu_stat_read(cpu, name) (0)
#define emp_vcpu_stat_reset(cpu, name) do {} while (0)
#define emp_vcpu_stat_init(cpu) do {} while (0)

#define emp_els_stat_inc(emm, gpa, name) do {} while (0)
#define emp_els_stat_add(emm, gpa, name, val) do {} while (0)
#define __emp_els_stat_add(emm, order, name, val) do {} while (0)
#define emp_els_stat_read(emm, name, order) (0)
#define emp_els_stat_reset(emm, name, order) do {} while (0)
#endif /* !CONFIG_EMP_STAT */
#endif /* __STAT_H__ */
