#ifndef __LOCAL_PAGE_H__
#define __LOCAL_PAGE_H__

#include <asm/pgtable_types.h>
#include "debug.h"

struct mapped_pmd {
	struct mapped_pmd *next;
	pmd_t             *pmd;
	int               vmr_id;
};

#ifdef CONFIG_EMP_DEBUG_LRU_LIST
#define DEBUG_LRU_SIZE (32)
enum debug_lru_type {
	DEBUG_LRU_UNKNOWN = 0,
	DEBUG_LRU_SET_CPU = 1,
	DEBUG_LRU_SET_LIST = 2,
	DEBUG_LRU_CLEAR_LIST = 3,
	DEBUG_LRU_SET_VMR_ID = 4,
	DEBUG_LRU_ADD_VMR_ID = 5,
	DEBUG_LRU_DEL_VMR_ID = 6,
	DEBUG_LRU_PROGRESS = 7
};
#endif
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
#define DEBUG_PAGE_REF_SIZE (32)
struct debug_page_ref_history {
	char *file;
	int line;
	int add; // added reference count
	int curr; // current reference count after update
	int sum;
	int map;
	int vmr_id;
	int num_pmds;
	u64 timestamp;
	s8 in_mmu_noti;
	s8 in_io;
	s8 in_will_pte;
	s8 in_unmap;
	s8 in_dup;
	s8 in_calibrate;
};
#endif
#ifdef CONFIG_EMP_DEBUG_RSS
#ifdef CONFIG_EMP_DEBUG_RSS_PROGRESS
#define DEBUG_RSS_PROGRESS_SIZE (32)
struct debug_rss_progress {
	char *file;
	int line;
	int vmr_id;
	int is_add;
};
#endif
#endif

struct local_page {
	struct list_head  lru_list;
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	struct list_head elem;
	struct {
		char *file;
		int line;
		enum debug_lru_type type;
		long data;
	} debug_lru_history[DEBUG_LRU_SIZE];
	unsigned long debug_lru_next;
	struct emp_gpa *gpa;
#endif
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	struct debug_page_ref_history debug_page_ref_history[DEBUG_PAGE_REF_SIZE];
	int debug_page_ref_page_len;
	int debug_page_ref_next;
	int debug_page_ref_sum;
	s8 debug_page_ref_in_mmu_noti;
	s8 debug_page_ref_in_io;
	s8 debug_page_ref_in_will_pte;
	s8 debug_page_ref_in_unmap;
	s8 debug_page_ref_in_dup;
	s8 debug_page_ref_in_calibrate;
	int debug_page_ref_wrong;
#endif
	struct page       *page;
	u64               *sptep;
	int               flags;
	size_t            gpa_index;  // a backlink to gpa
	u64               dma_addr;
	int               vmr_id;
	int               num_pmds;
	struct mapped_pmd pmds;
	// for inactive queue for cpu and stat
	s16             cpu;
	// (fetched block on) demand offset used by csf
	// valid only if CSF flag is set
	u16             demand_offset; // on head
	struct work_request     *w;
#ifdef CONFIG_EMP_DEBUG_RSS
	u64 debug_rss_bitmap[DEBUG_RSS_BITMAP_U64LEN];
#ifdef CONFIG_EMP_DEBUG_RSS_PROGRESS
	struct debug_rss_progress debug_rss_progress[DEBUG_RSS_PROGRESS_SIZE];
	int debug_rss_progress_next;
#endif
#endif
};

static inline struct local_page *__get_local_page_from_list(struct list_head *p)
{
	return list_entry(p, struct local_page, lru_list);
}

#ifdef CONFIG_EMP_DEBUG_LRU_LIST
#define debug_lru_set_cpu_mark(lp, cpu_id) do { \
	unsigned long ____next = (lp)->debug_lru_next; \
	(lp)->debug_lru_history[____next].file = __FILE__; \
	(lp)->debug_lru_history[____next].line = __LINE__; \
	(lp)->debug_lru_history[____next].type = DEBUG_LRU_SET_CPU; \
	(lp)->debug_lru_history[____next].data = (long) (cpu_id); \
	(lp)->debug_lru_next = ((lp)->debug_lru_next + 1) % DEBUG_LRU_SIZE; \
} while (0)
#define debug_lru_set_list_mark(lp) do { \
	unsigned long ____next = (lp)->debug_lru_next; \
	(lp)->debug_lru_history[____next].file = __FILE__; \
	(lp)->debug_lru_history[____next].line = __LINE__; \
	(lp)->debug_lru_history[____next].type = DEBUG_LRU_SET_LIST; \
	(lp)->debug_lru_history[____next].data = 0UL; \
	(lp)->debug_lru_next = ((lp)->debug_lru_next + 1) % DEBUG_LRU_SIZE; \
} while (0)
#define debug_lru_clear_list_mark(lp) do { \
	unsigned long ____next = (lp)->debug_lru_next; \
	(lp)->debug_lru_history[____next].file = __FILE__; \
	(lp)->debug_lru_history[____next].line = __LINE__; \
	(lp)->debug_lru_history[____next].type = DEBUG_LRU_CLEAR_LIST; \
	(lp)->debug_lru_history[____next].data = 0UL; \
	(lp)->debug_lru_next = ((lp)->debug_lru_next + 1) % DEBUG_LRU_SIZE; \
} while (0)
#define debug_lru_set_vmr_id_mark(lp, vmr_id) do { \
	unsigned long ____next = (lp)->debug_lru_next; \
	(lp)->debug_lru_history[____next].file = __FILE__; \
	(lp)->debug_lru_history[____next].line = __LINE__; \
	(lp)->debug_lru_history[____next].type = DEBUG_LRU_SET_VMR_ID; \
	(lp)->debug_lru_history[____next].data = (long) (vmr_id); \
	(lp)->debug_lru_next = ((lp)->debug_lru_next + 1) % DEBUG_LRU_SIZE; \
} while (0)
#define debug_lru_add_vmr_id_mark(lp, vmr_id) do { \
	unsigned long ____next = (lp)->debug_lru_next; \
	(lp)->debug_lru_history[____next].file = __FILE__; \
	(lp)->debug_lru_history[____next].line = __LINE__; \
	(lp)->debug_lru_history[____next].type = DEBUG_LRU_ADD_VMR_ID; \
	(lp)->debug_lru_history[____next].data = (long) (vmr_id); \
	(lp)->debug_lru_next = ((lp)->debug_lru_next + 1) % DEBUG_LRU_SIZE; \
} while (0)
#define debug_lru_del_vmr_id_mark(lp, vmr_id) do { \
	unsigned long ____next = (lp)->debug_lru_next; \
	(lp)->debug_lru_history[____next].file = __FILE__; \
	(lp)->debug_lru_history[____next].line = __LINE__; \
	(lp)->debug_lru_history[____next].type = DEBUG_LRU_DEL_VMR_ID; \
	(lp)->debug_lru_history[____next].data = (long) (vmr_id); \
	(lp)->debug_lru_next = ((lp)->debug_lru_next + 1) % DEBUG_LRU_SIZE; \
} while (0)
#define debug_lru_progress_mark(lp, val) do { \
	unsigned long ____next = (lp)->debug_lru_next; \
	(lp)->debug_lru_history[____next].file = __FILE__; \
	(lp)->debug_lru_history[____next].line = __LINE__; \
	(lp)->debug_lru_history[____next].type = DEBUG_LRU_PROGRESS; \
	(lp)->debug_lru_history[____next].data = (long) (val); \
	(lp)->debug_lru_next = ((lp)->debug_lru_next + 1) % DEBUG_LRU_SIZE; \
} while (0)
#else
#define debug_lru_set_cpu_mark(lp, cpu_id) do {} while (0)
#define debug_lru_set_list_mark(lp) do {} while (0)
#define debug_lru_clear_list_mark(lp) do {} while (0)
#define debug_lru_set_vmr_id_mark(lp, vmr_id) do {} while (0)
#define debug_lru_add_vmr_id_mark(lp, vmr_id) do {} while (0)
#define debug_lru_del_vmr_id_mark(lp, vmr_id) do {} while (0)
#define debug_lru_progress_mark(lp, data) do {} while (0)
#endif

#ifdef CONFIG_EMP_DEBUG_PAGE_REF
/* NOTE: we track only the first page. we assume compound pages */
#define debug_page_mapcount(p) (atomic_read(&(p)->_mapcount) + 1)
static inline bool is_debug_page_ref_correct(struct local_page *lp, int *refcnt, int *mapcnt, int known_diff) {
	int correct;
	int ref, map;
	bool ret = false;
	u64 t = 0;
retry:
	ref = page_count(lp->page) + known_diff;
	/* we simply read _mapcount value to use this on pro module. */ \
	map = debug_page_mapcount(lp->page);
	correct = 1 + (map * lp->debug_page_ref_page_len)
			+ lp->debug_page_ref_in_mmu_noti
			+ lp->debug_page_ref_in_io
			+ lp->debug_page_ref_in_will_pte
			+ lp->debug_page_ref_in_calibrate;
	if (ref == correct) {
		ret = true;
		goto out;
	}
	if (lp->debug_page_ref_in_unmap > 0
			&& (ref - correct) > 0
			&& (ref - correct) <= lp->debug_page_ref_in_unmap) {
		ret = true;
		goto out;
	}
	if (lp->debug_page_ref_in_dup > 0
			&& (correct - ref) > 0
			&& (correct - ref) <= lp->debug_page_ref_in_dup) {
		ret = true;
		goto out;
	}
	if (t == 0) {
		t = get_ts_in_ns();
		cond_resched();
		goto retry;
	} else if (get_ts_in_ns() - t < 1000000000) { // Wait up to 1s
		cond_resched();
		goto retry;
	}
out:
	*refcnt = ref - known_diff;
	*mapcnt = map;
	return ret;
}

static inline void debug_page_ref_print_all(struct local_page *lp)
{
	emp_debug_bulk_msg_lock();
	__debug_page_ref_print_all(lp);
	emp_debug_bulk_msg_unlock();
}

#define __debug_page_ref_mark(vmrid, lp, add_data, known_diff) do { \
	int ____next = (lp)->debug_page_ref_next; \
	int ____print_all = 0; \
	int ____ref, ____map; \
	(lp)->debug_page_ref_sum += (add_data); \
	if (!is_debug_page_ref_correct(lp, &____ref, &____map, known_diff)) { \
		(lp)->debug_page_ref_wrong++; \
		emp_debug_bulk_msg_lock(); \
		printk(KERN_ERR "DEBUG: [WRONG PAGE COUNT] lp: %016lx page_len: %d #wrong: %d " \
				"ref: %d map: %d sum: %d add: %d " \
				"mmu: %d io: %d pte: %d unmap: %d dup: %d calibrate: %d " \
				"(%s) %s:%d\n", \
				(unsigned long) (lp), \
				(lp)->debug_page_ref_page_len, \
				(lp)->debug_page_ref_wrong, \
				____ref, ____map, \
				(lp)->debug_page_ref_sum, \
				(int) (add_data), \
				(lp)->debug_page_ref_in_mmu_noti, \
				(lp)->debug_page_ref_in_io, \
				(lp)->debug_page_ref_in_will_pte, \
				(lp)->debug_page_ref_in_unmap, \
				(lp)->debug_page_ref_in_dup, \
				(lp)->debug_page_ref_in_calibrate, \
				__func__, __FILE__, __LINE__); \
		____print_all = 1; \
	} \
	(lp)->debug_page_ref_history[____next].file = __FILE__; \
	(lp)->debug_page_ref_history[____next].line = __LINE__; \
	(lp)->debug_page_ref_history[____next].add = (add_data); \
	(lp)->debug_page_ref_history[____next].curr = ____ref; \
	(lp)->debug_page_ref_history[____next].sum = (lp)->debug_page_ref_sum; \
	(lp)->debug_page_ref_history[____next].map = ____map; \
	(lp)->debug_page_ref_history[____next].vmr_id = (vmrid) != -100 ? (vmrid) : (lp)->vmr_id; \
	(lp)->debug_page_ref_history[____next].num_pmds = (lp)->num_pmds; \
	(lp)->debug_page_ref_history[____next].timestamp = get_ts_in_ns(); \
	(lp)->debug_page_ref_history[____next].in_mmu_noti = (lp)->debug_page_ref_in_mmu_noti; \
	(lp)->debug_page_ref_history[____next].in_io = (lp)->debug_page_ref_in_io; \
	(lp)->debug_page_ref_history[____next].in_will_pte = (lp)->debug_page_ref_in_will_pte; \
	(lp)->debug_page_ref_history[____next].in_unmap = (lp)->debug_page_ref_in_unmap; \
	(lp)->debug_page_ref_history[____next].in_dup = (lp)->debug_page_ref_in_dup; \
	(lp)->debug_page_ref_history[____next].in_calibrate = (lp)->debug_page_ref_in_calibrate; \
	(lp)->debug_page_ref_next = ((lp)->debug_page_ref_next + 1) % DEBUG_PAGE_REF_SIZE; \
	if (____print_all) { \
		__debug_page_ref_print_all(lp); \
		emp_debug_bulk_msg_unlock(); \
	} \
} while (0)
#define __debug_page_ref_mark_page(vmrid, p, add_data, known_diff) do { \
	struct local_page *____lp = ((struct emp_gpa *)(p)->private)->local_page; \
	debug_assert(____lp->page == (p)); \
	__debug_page_ref_mark(vmrid, ____lp, add_data, known_diff); \
} while (0)
#define debug_page_ref_mark(vmrid, lp, add_data) __debug_page_ref_mark(vmrid, lp, add_data, 0);
#define debug_page_ref_mark_known_diff(vmrid, lp, add_data, diff) __debug_page_ref_mark(vmrid, lp, add_data, diff);
#define debug_page_ref_mark_page(vmrid, p, add_data) __debug_page_ref_mark_page(vmrid, p, add_data, 0);
#define debug_page_ref_mark_page_known_diff(vmrid, p, known_diff) __debug_page_ref_mark_page(vmrid, p, add_data, diff);

#define __debug_page_ref_update_page_len(lp, page_len) do { \
	(lp)->debug_page_ref_page_len = (page_len); \
} while (0)

#define debug_page_ref_update_page_len(lp, vmr, gpa, gpa_idx) do { \
	__debug_page_ref_update_page_len(lp, __gpa_to_page_len(vmr, gpa, gpa_idx)); \
} while (0)

#define debug_page_ref_mmu_noti_beg(lp) do { (lp)->debug_page_ref_in_mmu_noti++; } while (0)
#define debug_page_ref_mmu_noti_end(lp) do { (lp)->debug_page_ref_in_mmu_noti--; } while (0)
#define debug_page_ref_io_beg(lp) do { (lp)->debug_page_ref_in_io++; } while (0)
#define debug_page_ref_io_end(lp) do { (lp)->debug_page_ref_in_io--; } while (0)
#define debug_page_ref_will_pte_beg(lp, len) do { (lp)->debug_page_ref_in_will_pte += (len); } while (0)
#define debug_page_ref_will_pte_end(lp, len) do { (lp)->debug_page_ref_in_will_pte -= (len); } while (0)
#define debug_page_ref_unmap_beg(lp) do { (lp)->debug_page_ref_in_unmap++; } while (0)
#define debug_page_ref_unmap_end(lp) do { (lp)->debug_page_ref_in_unmap--; } while (0)
#define debug_page_ref_dup_beg(lp) do { (lp)->debug_page_ref_in_dup++; } while (0)
#define debug_page_ref_dup_end(lp) do { (lp)->debug_page_ref_in_dup--; } while (0)
#define debug_page_ref_calibrate_beg(lp, len) do { (lp)->debug_page_ref_in_calibrate += (len); } while (0)
#define debug_page_ref_calibrate_end(lp, len) do { (lp)->debug_page_ref_in_calibrate -= (len); } while (0)
/* for handle_emp_cow_fault_mmu() */
#define debug_page_ref_mmu_noti_beg_safe(lp) do { if (lp) debug_page_ref_mmu_noti_beg_careful(lp); } while (0)
#define debug_page_ref_mmu_noti_end_safe(lp) do { if (lp) debug_page_ref_mmu_noti_end_careful(lp); } while (0)
#define debug_page_ref_mark_safe(vmrid, lp, add_data) do { if (lp) debug_page_ref_mark(vmrid, lp, add_data); } while (0)
#define debug_page_ref_mark_known_diff_safe(vmrid, lp, add_data, diff) do { if (lp) debug_page_ref_mark_known_diff(vmrid, lp, add_data, diff); } while (0)
#else
#define debug_page_ref_mark(vmrid, lp, add_data) do {} while (0)
#define debug_page_ref_mark_known_diff(vmrid, lp, add_data, diff) do {} while (0)
#define debug_page_ref_mark_page(vmrid, p, add_data) do {} while (0)
#define debug_page_ref_mark_page_known_diff(vmrid, p, add_data, diff) do {} while (0)
#define __debug_page_ref_update_page_len(lp, page_len) do {} while (0)
#define debug_page_ref_update_page_len(lp, vmr, gpa, gpa_index) do {} while (0)
#define debug_page_ref_mmu_noti_beg(lp) do {} while (0)
#define debug_page_ref_mmu_noti_end(lp) do {} while (0)
#define debug_page_ref_io_beg(lp) do {} while (0)
#define debug_page_ref_io_end(lp) do {} while (0)
#define debug_page_ref_will_pte_beg(lp, len) do {} while (0)
#define debug_page_ref_will_pte_end(lp, len) do {} while (0)
#define debug_page_ref_unmap_beg(lp) do {} while (0)
#define debug_page_ref_unmap_end(lp) do {} while (0)
#define debug_page_ref_dup_beg(lp) do {} while (0)
#define debug_page_ref_dup_end(lp) do {} while (0)
#define debug_page_ref_calibrate_beg(lp, len) do {} while (0)
#define debug_page_ref_calibrate_end(lp, len) do {} while (0)
/* for handle_emp_cow_fault_mmu() */
#define debug_page_ref_mmu_noti_beg_safe(lp) do {} while (0)
#define debug_page_ref_mmu_noti_end_safe(lp) do {} while (0)
#define debug_page_ref_mark_safe(vmrid, lp, add_data) do {} while (0)
#define debug_page_ref_mark_known_diff_safe(vmrid, lp, add_data, diff) do {} while (0)
#endif

enum local_page_flags {
	// when lp->lru_list is on proactive or active list, which list it belongs to
	// ON_MRU is also used for inactive list and ON_LRU is also used for
	// writeback list
	LOCAL_PAGE_ON_MRU,
	LOCAL_PAGE_ON_LRU,
	LOCAL_PAGE_ON_GLOBAL,
};

#define LOCAL_PAGE_ON_MRU_MASK (1 << LOCAL_PAGE_ON_MRU)
#define LOCAL_PAGE_ON_LRU_MASK (1 << LOCAL_PAGE_ON_LRU)
#define LOCAL_PAGE_ON_GLOBAL_MASK (1 << LOCAL_PAGE_ON_GLOBAL)
#define LOCAL_PAGE_ON_LIST_MASK (LOCAL_PAGE_ON_MRU_MASK \
				| LOCAL_PAGE_ON_LRU_MASK \
				| LOCAL_PAGE_ON_GLOBAL_MASK)

#define ____is_local_page_flags(lp, flag_idx) ((lp)->flags & (1 << (flag_idx)))
#define ____set_local_page_flags(lp, flag_idx) do { \
	(lp)->flags = (lp)->flags | (1 << (flag_idx)); \
} while (0)
#define ____clear_local_page_flags(lp, flag_idx) do { \
	(lp)->flags = (lp)->flags & (~(1 << (flag_idx))); \
} while (0)
#define ____set_local_page_list_flags(lp, flag_idx) do { \
	debug_assert((flag_idx) == LOCAL_PAGE_ON_MRU \
			|| (flag_idx) == LOCAL_PAGE_ON_LRU \
			|| (flag_idx) == LOCAL_PAGE_ON_GLOBAL); \
	(lp)->flags = ((lp)->flags & (~(LOCAL_PAGE_ON_LIST_MASK))) \
					| (1 << (flag_idx)); \
	debug_lru_set_list_mark(lp); \
} while (0)

#define is_local_page_on_mru(lp) ____is_local_page_flags(lp, LOCAL_PAGE_ON_MRU)
#define is_local_page_on_lru(lp) ____is_local_page_flags(lp, LOCAL_PAGE_ON_LRU)
#define is_local_page_on_global(lp) ____is_local_page_flags(lp, LOCAL_PAGE_ON_GLOBAL)
#define is_local_page_on_list(lp) ((lp)->flags & LOCAL_PAGE_ON_LIST_MASK)

#define set_local_page_on_mru(lp) ____set_local_page_list_flags(lp, LOCAL_PAGE_ON_MRU)
#define set_local_page_on_lru(lp) ____set_local_page_list_flags(lp, LOCAL_PAGE_ON_LRU)
#define set_local_page_on_global(lp) ____set_local_page_list_flags(lp, LOCAL_PAGE_ON_GLOBAL)

#define clear_local_page_on_mru(lp) ____clear_local_page_flags(lp, LOCAL_PAGE_ON_MRU)
#define clear_local_page_on_lru(lp) ____clear_local_page_flags(lp, LOCAL_PAGE_ON_LRU)
#define clear_local_page_on_global(lp) ____clear_local_page_flags(lp, LOCAL_PAGE_ON_GLOBAL)
#define clear_local_page_list_flags(lp) do { \
	(lp)->flags = (lp)->flags & (~(LOCAL_PAGE_ON_LIST_MASK)); \
	debug_lru_clear_list_mark(lp); \
} while (0)

#define get_local_page_cpu(lp) ((lp)->cpu)
#define __set_local_page_cpu_list(lp, cpu_id, flag_idx) do { \
		(lp)->cpu = (cpu_id); \
		debug_lru_set_cpu_mark(lp, cpu_id); \
		____set_local_page_list_flags(lp, flag_idx); \
} while (0)
#define set_local_page_cpu_only(lp, cpu_id) do { \
		(lp)->cpu = (cpu_id); \
		debug_lru_set_cpu_mark(lp, cpu_id); \
		clear_local_page_list_flags(lp); \
} while (0)
#define set_local_page_cpu_mru(lp, cpu_id) __set_local_page_cpu_list(lp, cpu_id, LOCAL_PAGE_ON_MRU)
#define set_local_page_cpu_lru(lp, cpu_id) __set_local_page_cpu_list(lp, cpu_id, LOCAL_PAGE_ON_LRU)
#define set_local_page_global(lp) ____set_local_page_list_flags(lp, LOCAL_PAGE_ON_GLOBAL)

#define set_lru_elem_cpu_mru(list, cpu_id) set_local_page_cpu_mru(__get_local_page_from_list(list), cpu_id)
#define set_lru_elem_cpu_lru(list, cpu_id) set_local_page_cpu_lru(__get_local_page_from_list(list), cpu_id)
#define set_lru_elem_global(list) set_local_page_global(__get_local_page_from_list(list))

#define __set_local_page_cpu_flag_list(list, cpu_id, flag_idx) do { \
		struct list_head *____cur; \
		struct local_page *____lp; \
		list_for_each(____cur, list) { \
			____lp = __get_local_page_from_list(____cur); \
			__set_local_page_cpu_list(____lp, cpu_id, flag_idx); \
		} \
} while (0)
#define __set_local_page_flag_list(list, flag_idx) do { \
		struct list_head *____cur; \
		struct local_page *____lp; \
		list_for_each(____cur, list) { \
			____lp = __get_local_page_from_list(____cur); \
			____set_local_page_list_flags(____lp, flag_idx); \
		} \
} while (0)
#define set_local_page_cpu_mru_list(list, cpu_id) __set_local_page_cpu_flag_list(list, cpu_id, LOCAL_PAGE_ON_MRU)
#define set_local_page_cpu_lru_list(list, cpu_id) __set_local_page_cpu_flag_list(list, cpu_id, LOCAL_PAGE_ON_LRU)
#define set_local_page_global_list(list) __set_local_page_flag_list(list, LOCAL_PAGE_ON_GLOBAL)

#define INIT_MAPPED_PMD(p) { \
	(p)->next = NULL; \
	(p)->pmd = NULL; \
	(p)->vmr_id = -1; \
}

static inline struct mapped_pmd *emp_lp_alloc_pmd(struct emp_mm *emm) {
	return emp_kmem_cache_alloc(emm->ftm.mapped_pmd_cache, GFP_KERNEL);
}

static inline void emp_lp_free_pmd(struct emp_mm *emm, struct mapped_pmd *p) {
	emp_kmem_cache_free(emm->ftm.mapped_pmd_cache, p);
}

bool emp_lp_lookup_pmd(struct local_page *, int, struct mapped_pmd **,
		       struct mapped_pmd **);

static inline pmd_t *emp_lp_get_pmd(struct local_page *lp, int vmr_id) {
	struct mapped_pmd *p, *pp;
	if (emp_lp_lookup_pmd(lp, vmr_id, &p, &pp))
		return p->pmd;
	else
		return NULL;
}


bool emp_lp_insert_pmd(struct emp_mm *, struct local_page *, int, pmd_t *);

pmd_t *emp_lp_pop_pmd(struct emp_mm *, struct local_page *, int);

static inline bool
emp_lp_remove_pmd(struct emp_mm *emm, struct local_page *lp, int vmr_id)
{
	return emp_lp_pop_pmd(emm, lp, vmr_id) ? true : false;
}

#define EMP_LP_PMDS_EMPTY(p) ((p)->next == NULL)
#define EMP_LP_PMDS_SINGLE(p) ((p)->next == (p))

static inline int emp_lp_count_pmd(struct local_page *lp) {
	debug_emp_lp_count_pmd(lp);
	return lp->num_pmds;
}

int local_page_init(struct emp_mm *);
void local_page_exit(struct emp_mm *);

#endif
