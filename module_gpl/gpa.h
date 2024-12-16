#ifndef __GPA_H__
#define __GPA_H__

#include <linux/kvm_types.h>
#include <linux/mm_types.h>
#include "config.h"
#include "debug_gpa.h"
#include "emp_type.h"

struct emp_mm;
struct emp_vmr;

enum gpa_flags {
	GPA_touched = 0,
	GPA_dirty = 1,
	GPA_access = 2,
	GPA_ept = 3,
	GPA_hpt = 4,
	GPA_remote = 5,
	GPA_prefetched_csf = 6,
	GPA_prefetched_cpf = 7,
	GPA_prefetch_once = 8,
	GPA_referenced = 9,
#ifdef CONFIG_EMP_VM
	GPA_lowmemory_block = 10,
#endif
	GPA_stale_block = 11,
#ifdef CONFIG_EMP_IO
	GPA_io_read_page = 13,
	GPA_io_write_page = 14,
	GPA_io_in_progress = 15,
#endif
	NUM_GPA_FLAGS = 16,
};

#define GPA_TOUCHED_MASK    (1 << GPA_touched)
#define GPA_DIRTY_MASK      (1 << GPA_dirty)
#define GPA_ACCESS_MASK     (1 << GPA_access)
#ifdef CONFIG_EMP_VM
#define GPA_EPT_MASK        (1 << GPA_ept)
#define GPA_HPT_MASK        (1 << GPA_hpt)
#define GPA_nPT_MASK        (GPA_EPT_MASK | GPA_HPT_MASK)
#else /* !CONFIG_EMP_VM */
#define GPA_HPT_MASK        (1 << GPA_hpt)
#define GPA_nPT_MASK        (GPA_HPT_MASK)
#endif /* !CONFIG_EMP_VM */
#define GPA_REMOTE_MASK     (1 << GPA_remote)
#define GPA_PREFETCHED_CSF_MASK  (1 << GPA_prefetched_csf)
#define GPA_PREFETCHED_CPF_MASK  (1 << GPA_prefetched_cpf)
#define GPA_PREFETCHED_MASK  (GPA_PREFETCHED_CSF_MASK | GPA_PREFETCHED_CPF_MASK)
#define GPA_PREFETCH_ONCE_MASK  (1 << GPA_prefetch_once)
#define GPA_REFERENCED_MASK (1 << GPA_referenced)
#ifdef CONFIG_EMP_IO
#define GPA_IO_READ_MASK    (1 << GPA_io_read_page)
#define GPA_IO_WRITE_MASK   (1 << GPA_io_write_page)
#define GPA_IO_IP_MASK      (1 << GPA_io_in_progress) // IO In Progress
#define GPA_IO_MASK         (GPA_IO_READ_MASK | GPA_IO_WRITE_MASK | \
                                GPA_IO_IP_MASK)
#endif
#ifdef CONFIG_EMP_VM
#define GPA_LOWMEM_BLOCK_MASK (1 << GPA_lowmemory_block)
#endif
#define GPA_STALE_BLOCK_MASK (1 << GPA_stale_block)

#ifdef CONFIG_EMP_VM
#define GPA_KEEP_FLAGS_MASK \
	(GPA_TOUCHED_MASK | GPA_LOWMEM_BLOCK_MASK)
#else /* !CONFIG_EMP_VM */
#define GPA_KEEP_FLAGS_MASK \
	(GPA_TOUCHED_MASK | GPA_PARTIAL_MAP_MASK)
#endif /* !CONFIG_EMP_VM */
#define GPA_CLEANUP_MASK ((1 << NUM_GPA_FLAGS) - 1) & (~GPA_KEEP_FLAGS_MASK)

/*
 * enum gpa_state
 *
 * GPA_INIT: initialized
 * GPA_ACTIVE: GPA in active list
 * GPA_ACTIVE_IN_TRANSITION: GPA in active list but in transition.
 *     This state is introduced for differentiating existing gpas
 *     and gpas just fetched in emp_fetch_barrier and emp_install_sptes. 
 * GPA_INACTIVE: GPA in inactive list
 * GPA_WB: GPA in writeback status (in any list)
 * GPA_FETCHING: GPA in fetching status (in any list)
 * GPA_TRANS: transient state between two lists (hidden state)
 */
enum gpa_state {
	GPA_INIT = 0,
	GPA_ACTIVE = 1,
	GPA_INACTIVE = 2,
	GPA_WB = 3,
	GPA_FETCHING = 4, // vcpu_threads except for io_thread
	GPA_TRANS_IL = 5, // for debugging
	GPA_TRANS_AL = 6, // for debugging
	GPA_TRANS_PL = 7, // for debugging
	GPA_STATE_MAX
};

#define INIT_BLOCK(b)     ((b)->r_state == GPA_INIT)
#define ACTIVE_BLOCK(b)   ((b)->r_state == GPA_ACTIVE)
#define INACTIVE_BLOCK(b) ((b)->r_state == GPA_INACTIVE)
#define WB_BLOCK(b)       ((b)->r_state == GPA_WB)
#define FETCHING_BLOCK(b) ((b)->r_state == GPA_FETCHING)

#define EMP_UNKNOWN_CPU_ID (SHRT_MIN) /* gpa->cpu is a signed 2-byte value */

#define ERR_SYSTEM_MAP (0)
#define ERR_MAP_EXISTS (1)


#ifdef CONFIG_EMP_BLOCK
#define gpa_subblock_order(g) ((g)->sb_order)
#define gpa_subblock_size(g)  (1 << (g)->sb_order)
#define gpa_subblock_mask(g)  (gpa_subblock_size(g) - 1)
#else
#define gpa_subblock_order(g) (0)
#define gpa_subblock_size(g)  (1)
#define gpa_subblock_mask(g)  (0)
#endif

#define gpa_subblock_page_order(g) (gpa_subblock_order(g) + PAGE_SHIFT)
#define gpa_subblock_page_size(g)  (1 << gpa_subblock_page_order(g))
#define gpa_subblock_page_mask(g)  (gpa_subblock_page_size(g) - 1)

static inline unsigned int get_gpa_flags(struct emp_gpa *gpa)
{
	return gpa->flags;
}

static inline bool is_gpa_flags_set(struct emp_gpa *gpa, unsigned int mask)
{
	return (gpa->flags & mask);
}

static inline bool is_gpa_flags_same(struct emp_gpa *gpa, unsigned int mask,
		unsigned int value)
{
	return (gpa->flags & mask) == value;
}

static inline void __init_gpa_flags(struct emp_gpa *gpa, unsigned int flag)
{
	gpa->flags = flag;
}

static inline void __set_gpa_flags(struct emp_gpa *gpa, unsigned int flag)
{
	gpa->flags |= flag;
}

/* return the previous value */
static inline bool __set_gpa_flags_if_unset(struct emp_gpa *gpa, unsigned int mask)
{
	if (!is_gpa_flags_same(gpa, mask, mask)) {
		gpa->flags |= mask;
		return false;
	}
	return true;
}

/* return the previous value */
static inline bool __clear_gpa_flags_if_set(struct emp_gpa *gpa, unsigned int mask)
{
	if (!is_gpa_flags_same(gpa, mask, 0)) {
		gpa->flags &= ~mask;
		return true;
	}
	return false;
}

#define init_gpa_flags(gpa, flag) do { \
	debug_progress_flag(gpa, flag); \
	__init_gpa_flags(gpa, flag); \
} while (0)

#define set_gpa_flags(gpa, flag) do { \
	debug_progress_flag(gpa, flag); \
	__set_gpa_flags(gpa, flag); \
} while (0)

#define set_gpa_flags_if_unset(gpa, mask) ({ \
	debug_progress_flag(gpa, mask); \
	__set_gpa_flags_if_unset(gpa, mask); \
})

#define clear_gpa_flags_if_set(gpa, mask) ({ \
	debug_progress_flag(gpa, mask); \
	__clear_gpa_flags_if_set(gpa, mask); \
})

/* get gpa descriptor of the given index in vmr */
struct emp_gpa *new_gpadesc(struct emp_vmr *vmr, unsigned long index);
#define raw_get_gpadesc(vmr, index) ((vmr)->descs->gpa_dir[index])

#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
#define get_gpadesc(vmr, index) ({ \
	struct emp_gpa *____gpa = raw_get_gpadesc(vmr, index); \
	if (!____gpa) { \
		____gpa = (vmr)->new_gpadesc(vmr, index); \
		if (likely(____gpa)) \
			(vmr)->set_gpadesc_alloc_at(vmr, index, __FILE__, __LINE__); \
	} \
	____gpa; \
})
#else
#define get_gpadesc(vmr, index) \
	(raw_get_gpadesc(vmr, index) \
			? raw_get_gpadesc(vmr, index) \
			: (vmr)->new_gpadesc(vmr, index))
#endif

#define get_exist_gpadesc(vmr, index) ({ \
	debug_BUG_ON((index) >= (vmr)->descs->gpa_len); \
	debug_BUG_ON(!raw_get_gpadesc(vmr, index)); \
	raw_get_gpadesc(vmr, index); \
})

#define get_local_gpa_index(gpa) ({ \
	debug_BUG_ON(!(gpa)); \
	debug_BUG_ON(!(gpa)->local_page); \
	(gpa)->local_page->gpa_index; \
})

#ifdef CONFIG_EMP_PREFER_DCPMM
#define alloc_gpadesc(emm, desc_order) ({ \
	size_t ____size = sizeof(struct emp_gpa) << (desc_order); \
	struct emp_gpa *____r = __vmalloc_node(____size, ____size, \
				__GFP_HIGHMEM | __GFP_MOVABLE | GFP_KERNEL, \
					2, __builtin_return_address(0)); \
	____r; \
})

#define free_gpadesc(emm, desc_order, head) vfree(head)
#else
/* alloc_gpadesc() and free_gpadesc() are defined as macros
 * for easy debugging */
#define alloc_gpadesc(emm, desc_order) ({ \
	struct kmem_cache *____c = get_gpadesc_alloc(emm, desc_order); \
	struct emp_gpa *____r = emp_kmem_cache_alloc(____c, GFP_KERNEL); \
	debug_BUG_ON(____r != NULL && ((unsigned long) ____r) % (sizeof(struct emp_gpa) * (1UL << (desc_order))) != 0);\
	____r; \
})

#define free_gpadesc(emm, desc_order, head) do { \
	struct kmem_cache *____c = get_gpadesc_alloc(emm, desc_order); \
	emp_kmem_cache_free(____c, head); \
} while (0)
#endif

#ifdef CONFIG_EMP_DEBUG_LRU_LIST
static inline void init_gpa_contrib_inactive(struct emp_gpa *g)
{
	g->contrib_inactive_len = 0;
	g->contrib_last_file = NULL;
	g->contrib_last_line = 0;
	g->contrib_last_val = 0;
}
#else
#define init_gpa_contrib_inactive(g) do {} while (0)
#endif

#ifdef CONFIG_EMP_VM
bool __kvm_flush_remote_tlbs(struct emp_mm *, bool);
u64 hva_to_gpa(struct emp_mm *, u64, struct kvm_memory_slot **);
#endif

struct emp_vmdesc *alloc_vmdesc(struct emp_vmdesc *prev);
int gpas_open(struct emp_vmr *);
void gpas_close(struct emp_vmr *, bool);
int gpa_init(struct emp_mm *);
void gpa_exit(struct emp_mm *);

#endif /* __GPA_H__ */
