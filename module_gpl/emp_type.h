#ifndef __EMP_TYPE_H__
#define __EMP_TYPE_H__
#include <linux/types.h>
#include <linux/spinlock_types.h>
#include <asm/pgtable_types.h>
#include "config.h"
#include "debug_gpa.h"

#define max2(a, b) (((a) > (b)) ? (a) : (b))

#define FREE_REMOTE_PAGE_VAL     (0xfffffffffffffffcULL)
#define REMOTE_PAGE_NONFLAG_MASK (0xfffffffffffffffcULL)
#define REMOTE_PAGE_FLAG_MASK    (0x0000000000000003ULL)
#define REMOTE_PAGE_MRID_MASK    (0xff00000000000000ULL)
#define REMOTE_PAGE_OFFSET_MASK  (0x00fffffffffffffcULL)
#define REMOTE_PAGE_MRID_SHIFT   (56)
#define REMOTE_PAGE_OFFSET_SHIFT (2)
#define REMOTE_PAGE_COW_MASK     (0x0000000000000001ULL)
#define REMOTE_PAGE_FREED_MASK   (0x0000000000000002ULL)
struct remote_page {
	/* layout of remote page value
	 * [63:56] memory region id, that is, offset in emm->memregs[id]
	 *         id 255 is reserved for invalid remote page
	 * [55:2]  page offset
	 * [1]     freed bit. if set, gpa freed this remote page.
	 * [0]     cow bit. if set, val & REMOTE_PAGE_NONFLAG_MASK is a pointer
	 *         to struct cow_remote_page
	 */
#ifdef CONFIG_EMP_DEBUG
	union {
		u64            val;
		void           *ptr; /* To show hexadecimal value in gdb */
	};
#else
	u64                    val;
#endif
};


#define WRONG_GPA_IDX ULONG_MAX

/* sizeof(struct emp_gpa) should be
 * 1. minimized
 * 2. cache line (64-byte) aligned.
 * 3. power-of-two: assumption of allocate_gpas()
 */
#define max2(a, b) (((a) > (b)) ? (a) : (b))
#ifdef CONFIG_EMP_DEBUG_INTEGRITY
#define EMP_GPA_STRUCT_TAG_SIZE (sizeof(u64))
#else
#define EMP_GPA_STRUCT_TAG_SIZE (0)
#endif
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
#define EMP_GPA_STRUCT_ALLOC_AT_SIZE (sizeof(struct gpadesc_alloc_at))
#else
#define EMP_GPA_STRUCT_ALLOC_AT_SIZE (0)
#endif
#ifdef CONFIG_EMP_DEBUG_PROGRESS
#define EMP_GPA_STRUCT_PROGRESS_SIZE (sizeof(struct progress_info))
#else
#define EMP_GPA_STRUCT_PROGRESS_SIZE (0)
#endif
#ifdef CONFIG_EMP_DEBUG_GPA_REFCNT
#define EMP_GPA_STRUCT_DEBUG_GPA_REFCNT_SIZE (sizeof(struct debug_gpa_refcnt))
#else
#define EMP_GPA_STRUCT_DEBUG_GPA_REFCNT_SIZE (0)
#endif
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
#define EMP_GPA_STRUCT_DEBUG_LRU_LIST_SIZE (sizeof(int) * 3 + sizeof(char *))
#else
#define EMP_GPA_STRUCT_DEBUG_LRU_LIST_SIZE (0)
#endif
#ifdef CONFIG_EMP_DEBUG
#define EMP_GPA_STRUCT_DEBUG_ETC (sizeof(u8))
#else
#define EMP_GPA_STRUCT_DEBUG_ETC (0)
#endif
#ifdef CONFIG_EMP_IO
#define EMP_GPA_STRUCT_IO (sizeof(unsigned int))
#else
#define EMP_GPA_STRUCT_IO (0)
#endif
#define EMP_GPA_STRUCT_SIZE (sizeof(spinlock_t) \
					+ sizeof(unsigned int) \
					+ sizeof(atomic_t) \
					+ 4 \
					+ sizeof(struct remote_page) \
					+ sizeof(struct local_page *) \
					+ EMP_GPA_STRUCT_IO \
					+ EMP_GPA_STRUCT_TAG_SIZE \
					+ EMP_GPA_STRUCT_ALLOC_AT_SIZE \
					+ EMP_GPA_STRUCT_PROGRESS_SIZE \
					+ EMP_GPA_STRUCT_DEBUG_GPA_REFCNT_SIZE \
					+ EMP_GPA_STRUCT_DEBUG_LRU_LIST_SIZE \
					+ EMP_GPA_STRUCT_DEBUG_ETC)

#define POWER_OF_TWO_PAD(a) (((a) > 4096) ? (8192 - (a)) \
				:((a) > 2048) ? (4096 - (a)) \
				 : ((a) > 1024) ? (2048 - (a)) \
				  : ((a) > 512) ? (1024 - (a)) \
				   : ((a) > 256) ? (512 - (a)) \
				    : ((a) > 128) ? (256 - (a)) \
				     : ((a) > 64) ? (128 - (a)) \
				      : ((a) > 32) ? (64 - (a)) \
				       : ((a) > 16) ? (32 - (a)) \
				        : (16 - (a)))
#define EMP_GPA_STRUCT_PAD_SIZE POWER_OF_TWO_PAD(EMP_GPA_STRUCT_SIZE)

struct emp_gpa {
	atomic_t                lock;
	unsigned int            flags; // refer to enum gpa_flags in gpa.h
	
	atomic_t                refcnt;

	// subblock order and block order of partial mapped block must be same
	struct { /* 4-byte */
		u8              sb_order:4;
		u8              block_order:4;
		u8		max_block_order:4;
		u8		desc_order:4;
		// for recording last memreg
		u8              last_mr_id;
		// r_state and cpu are updated together in add_gpas_to_inactive
		u8              r_state; // refer to enum gpa_state in gpa.h
	};	

	struct remote_page      remote_page;
	
	struct local_page       *local_page;

#ifdef CONFIG_EMP_IO
	unsigned int            pid;
#endif

#ifdef CONFIG_EMP_DEBUG
#ifdef CONFIG_EMP_DEBUG_INTEGRITY
	u64 tag;
#endif
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	struct gpadesc_alloc_at alloc_at;
#endif
#ifdef CONFIG_EMP_DEBUG_PROGRESS
	struct progress_info    progress;
#endif
#ifdef CONFIG_EMP_DEBUG_GPA_REFCNT
	struct debug_gpa_refcnt debug_gpa_refcnt;
#endif
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	int contrib_inactive_len;
	char *contrib_last_file;
	int contrib_last_line;
	int contrib_last_val;
#endif
	u8                      gfn_offset_order;
#endif

	uint8_t pad[EMP_GPA_STRUCT_PAD_SIZE];
} __attribute__ ((packed));

#endif /* __EMP_TYPE_H__ */
