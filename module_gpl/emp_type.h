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

#ifdef CONFIG_EMP_USER
struct cow_remote_page {
	atomic_t refcnt;
	struct remote_page remote_page;
};
#endif

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
	unsigned int            _flags; // refer to enum gpa_flags in gpa.h
	
	atomic_t                refcnt;

	// subblock order and block order of partial mapped block must be same
	struct { /* 4-byte */
#ifdef CONFIG_EMP_BLOCK
		u8              _sb_order:4;
		u8              _block_order:4;
		u8		_max_block_order:4;
		u8		_desc_order:4;
#endif
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

#define for_all_gpas_range(vmr, index, pos, start, end) \
	for ((index) = (start), (pos) = get_gpadesc(vmr, index); \
			(index) < (end); \
			(index)++, \
			(pos) = get_gpadesc(vmr, index))

#define raw_for_all_gpas_range(vmr, index, pos, start, end) \
	for ((index) = (start), (pos) = get_next_exist_gpadesc(vmr, &(index)); \
			(index) < (end); \
			(index)++, \
			(pos) = get_next_exist_gpadesc(vmr, &(index)))

#define for_all_gpas(vmr, index, pos) \
	for_all_gpas_range(vmr, index, pos, 0, (vmr)->descs->gpa_len)

#define raw_for_all_gpas(vmr, index, pos) \
	raw_for_all_gpas_range(vmr, index, pos, 0, (vmr)->descs->gpa_len)

#ifdef CONFIG_EMP_BLOCK
#define num_subblock_in_block(g) \
	(1 << (gpa_block_order(g) - gpa_subblock_order(g)))

#define for_each_gpas(pos, head) \
	for (pos = (head); \
		pos < ((head) + (num_subblock_in_block(head))); \
		pos++)

#define for_each_gpas_reverse(pos, head) \
	for (pos = ((head) + (num_subblock_in_block(head)) - 1); \
		pos >= (head); pos--)

#define for_each_gpas_index(pos, index, head) \
	for (pos = (head), index = 0;\
			pos < ((head) + (num_subblock_in_block(head)));\
			pos++, index++)

#define for_all_gpa_heads_range(vmr, index, pos, start, end) \
	for ((index) = emp_get_block_head_index(vmr, start), \
		(pos) = get_gpadesc(vmr, index); \
			(pos) && ((index) < (end)); \
			(index) += num_subblock_in_block(pos), \
			(pos) = get_gpadesc(vmr, index))

#define raw_for_all_gpa_heads_range(vmr, index, pos, start, end) \
	for ((index) = (start), \
	     (pos) = get_next_exist_head_gpadesc(vmr, &(index)); \
			(pos) && ((index) < (end)); \
			(index) += num_subblock_in_block(pos), \
			(pos) = get_next_exist_head_gpadesc(vmr, &(index)))

#define for_all_gpa_heads(vmr, index, pos) \
	for_all_gpa_heads_range(vmr, index, pos, 0, (vmr)->descs->gpa_len)

#define raw_for_all_gpa_heads(vmr, index, pos) \
	raw_for_all_gpa_heads_range(vmr, index, pos, 0, (vmr)->descs->gpa_len)

#define gpa_block_order(gpa) ((gpa)->_block_order)
#define gpa_block_size(gpa)	(1UL << gpa_block_order(gpa))
#define __gpa_block_size(gpa, order) (1UL << (gpa_block_order(gpa) + order))
#define gpa_block_mask(gpa)	(~(gpa_block_size(gpa) - 1))
#define gpa_block_offset(gpa, offset)	((offset) & (gpa_block_size(gpa) - 1))
#define gpa_page_mask(gpa)	~(__gpa_block_size(gpa, PAGE_SHIFT) - 1)

#define gpa_desc_order(gpa) ((gpa)->_desc_order)
#define gpa_desc_size(gpa) (1 << gpa_desc_order(gpa))
#define gpa_max_block_order(gpa) ((gpa)->_max_block_order)

#define __update_gpa_desc_order(gpa) do { \
		(gpa)->_desc_order = (gpa)->_block_order - (gpa)->_sb_order; \
} while (0)
#define set_gpa_block_order(gpa, order) do { \
		(gpa)->_block_order = (order); \
		__update_gpa_desc_order(gpa); \
} while (0)
#define inc_gpa_block_order(gpa) do { \
		(gpa)->_block_order++; \
		__update_gpa_desc_order(gpa); \
} while (0)
#define set_gpa_max_block_order(gpa, order) do { \
		(gpa)->_max_block_order = (order); \
} while (0)
#define copy_gpa_orders(dst, src) do { \
		(dst)->_sb_order = (src)->_sb_order; \
		(dst)->_block_order = (src)->_block_order; \
		(dst)->_max_block_order = (src)->_max_block_order; \
		(dst)->_desc_order = (src)->_desc_order; \
} while (0)
#else /* !CONFIG_EMP_BLOCK */

#define num_subblock_in_block(g) (1)

#define for_each_gpas(pos, head) \
	for (pos = (head); pos != NULL; pos = NULL)

#define for_each_gpas_reverse(pos, head) \
	for (pos = (head); pos != NULL; pos = NULL)

#define for_each_gpas_index(pos, index, head) \
	for (pos = (head), index = 0; pos != NULL; pos = NULL, index++)

#define for_all_gpa_heads_range(vmr, index, pos, start, end) \
	for_all_gpas_range(vmr, index, pos, start, end)

#define raw_for_all_gpa_heads_range(vmr, index, pos, start, end) \
	raw_for_all_gpas_range(vmr, index, pos, start, end)

#define for_all_gpa_heads(vmr, index, pos) \
	for_all_gpas(vmr, index, pos)

#define raw_for_all_gpa_heads(vmr, index, pos) \
	raw_for_all_gpas(vmr, index, pos)

#define gpa_block_order(gpa) (0)
#define gpa_block_size(gpa)	(1UL)
#define __gpa_block_size(gpa, order) (1UL << (order))
#define gpa_block_mask(gpa)	(~0UL)
#define gpa_block_offset(gpa, offset) (0)
#define gpa_page_mask(gpa) (~((1UL << PAGE_SHIFT) - 1))

#define gpa_desc_order(gpa) (0)
#define gpa_desc_size(gpa) (1)
#define gpa_max_block_order(gpa) (0)
#define set_gpa_block_order(gpa, order) debug_assert((order) == 0)
#define set_gpa_max_block_order(gpa, order) debug_assert((order) == 0)
/* The followings are not defined.
 * __update_gpa_desc_order(gpa)
 * inc_gpa_block_order(gpa)
 */
#define copy_gpa_orders(dst, src) do {} while (0)

#endif /* !CONFIG_EMP_BLOCK */

#endif /* __EMP_TYPE_H__ */
