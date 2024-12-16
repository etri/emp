#ifndef __REMOTE_PAGE_H__
#define __REMOTE_PAGE_H__

#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include "emp_type.h"
#include "vm.h"

#define MAX_MRID 254

/*
 * struct memreg is a descriptor for donated remote memory region
 * layout
 *  -----------------------------------------------------
 *  | alloc_zone -> incrementally grows  | free_zone   |
 *  -----------------------------------------------------
 *
 * alloc_zone contains remote pages which have been allocated once
 *  - it means that each page is allocated or exists on lfi structure
 * free_zone contains remote pages which are still not allocated
 * allocated is a partition of each zone and it can be used with
 * __atomic_add_unless
 */
struct memreg {
	int                     id;
	struct emp_mm           *bvma;
	struct connection       *conn;  // connection for a donor
	s64                     size;   // the number of pages in memregion

	atomic64_t              alloc_len; // the number of allocated pages
	unsigned long           *alloc_bitmap; // dma_order granularity
	unsigned long           free_start; // lowest offset of free pages
	unsigned long           alloc_next; // next offset of previous allocation
	spinlock_t              lock;

	u32                     addr;
	int                     port;

	void                    *ops; // struct dma_ops
	enum {
#ifdef CONFIG_EMP_RDMA
		MR_RDMA,
#endif
#ifdef CONFIG_EMP_BLOCKDEV
		MR_NVME,
#endif
#ifdef CONFIG_EMP_MEMDEV
		MR_MEM,
#endif
	} type;
	int                     dma_order;
	bool                    inorder;

	struct kmem_cache       *wr_cache;
	atomic_t                wr_len;
	enum {
		MR_STATE_NORMAL,
		MR_STATE_CLOSING,
	} state;
	wait_queue_head_t       wr_wq;
}; // struct memreg indicates a memory donor.

#ifdef CONFIG_EMP_RDMA
#define IS_MR_TYPE_RDMA(mr) ((mr)->type == (MR_RDMA))
#else
#define IS_MR_TYPE_RDMA(mr) (false)
#endif

/* struct remote_page defined in emp_type.h */
#ifdef CONFIG_EMP_USER
#define ____get_cow_rp(rp)  (&((struct cow_remote_page *) \
			((rp)->val & REMOTE_PAGE_NONFLAG_MASK))->remote_page)
#define ____get_rp_ptr(rp) (is_remote_page_cow(rp) ? ____get_cow_rp(rp) : (rp))
#else
#define ____get_rp_ptr(rp) (rp)
#endif

#define __get_remote_page_val(rp) ((rp)->val)
#define __get_gpa_remote_page_val(gpa) __get_remote_page_val(&(gpa)->remote_page)
#define __get_remote_page_mrid(rp) (((rp)->val & REMOTE_PAGE_MRID_MASK) \
						>> REMOTE_PAGE_MRID_SHIFT)
#define __get_remote_page_offset(rp) (((rp)->val & REMOTE_PAGE_OFFSET_MASK) \
						>> REMOTE_PAGE_OFFSET_SHIFT)
#define __set_remote_page(rp, rp_val) do { (rp)->val = (rp_val); } while (0)

static inline bool is_remote_page_free(struct remote_page *rp)
{
	/* if CoW remote page, it is not free */
	return rp->val == FREE_REMOTE_PAGE_VAL ? true : false;
}

static inline bool is_remote_page_cow(struct remote_page *rp)
{
	return (rp->val & REMOTE_PAGE_COW_MASK) ? true : false;
}

static inline bool is_remote_page_freed(struct remote_page *rp)
{
	// @rp belongs to work_request. Thus, it cannot be CoW remote page.
	debug_assert(is_remote_page_cow(rp) == false);
	return (rp->val & REMOTE_PAGE_FREED_MASK) ? true : false;
}

static inline struct cow_remote_page *
get_remote_page_cow(struct remote_page *rp)
{
	debug_assert(is_remote_page_cow(rp));
	return (struct cow_remote_page *) (rp->val & REMOTE_PAGE_NONFLAG_MASK);
}

static inline u64 get_remote_page_val(struct remote_page *rp)
{
	return __get_remote_page_val(____get_rp_ptr(rp));
}

static inline u8 get_remote_page_mrid(struct remote_page *rp)
{
	return __get_remote_page_mrid(____get_rp_ptr(rp));
}

static inline unsigned long get_remote_page_offset(struct remote_page *rp)
{
	return __get_remote_page_offset(____get_rp_ptr(rp));
}

static inline void set_remote_page(struct remote_page *rp, u64 val)
{
	debug_assert(is_remote_page_cow(rp) == false);
	debug_assert((val & REMOTE_PAGE_FLAG_MASK) == 0);
	__set_remote_page(rp, val);
}

static inline void set_remote_page_free(struct remote_page *rp)
{
	rp->val = FREE_REMOTE_PAGE_VAL;
}

static inline void set_remote_page_freed(struct remote_page *rp)
{
	// @rp belongs to work_request. Thus, it cannot be CoW remote page.
	debug_assert(is_remote_page_cow(rp) == false);
	rp->val = rp->val | REMOTE_PAGE_FREED_MASK;
}

static inline void
set_remote_page_cow(struct remote_page *rp, struct cow_remote_page *cow)
{
	debug_assert((((unsigned long) cow) & REMOTE_PAGE_FLAG_MASK) == 0UL);
	rp->val = (((unsigned long) cow) & REMOTE_PAGE_NONFLAG_MASK)
						| REMOTE_PAGE_COW_MASK;
}

static inline u64
make_remote_page_val(int mrid, unsigned long offset)
{
	return (((u64) mrid) << REMOTE_PAGE_MRID_SHIFT)
			| (((u64) offset) << REMOTE_PAGE_OFFSET_SHIFT);
}

static inline void
init_remote_page(struct remote_page *rp, int mrid, unsigned long offset)
{
	rp->val = make_remote_page_val(mrid, offset);
}

#define is_gpa_remote_page_cow(gpa) is_remote_page_cow(&(gpa)->remote_page)
#define is_gpa_remote_page_free(gpa) is_remote_page_free(&(gpa)->remote_page)
#define get_gpa_remote_page_val(gpa) get_remote_page_val(&(gpa)->remote_page)
#define get_gpa_remote_page_mrid(gpa) get_remote_page_mrid(&(gpa)->remote_page)
#define get_gpa_remote_page_offset(gpa) get_remote_page_offset(&(gpa)->remote_page)
#define get_gpa_remote_page_cow(gpa) get_remote_page_cow(&(gpa)->remote_page)
#define set_gpa_remote_page(gpa, val) set_remote_page(&(gpa)->remote_page, val)
#define set_gpa_remote_page_free(gpa) set_remote_page_free(&(gpa)->remote_page)
#define set_gpa_remote_page_cow(gpa, crp) set_remote_page_cow(&(gpa)->remote_page, crp)

#ifdef CONFIG_EMP_DEBUG_SHOW_GPA_STATE
static inline int get_gpa_remote_page_refcnt(struct emp_gpa *gpa)
{
#ifdef CONFIG_EMP_USER
	if (is_gpa_remote_page_cow(gpa))
		return atomic_read(&get_gpa_remote_page_cow(gpa)->refcnt);
	else
		return 0;
#else /* !CONFIG_EMP_USER */
	return 0;
#endif /* !CONFIG_EMP_USER */
}
#endif /* CONFIG_EMP_DEBUG_SHOW_GPA_STATE */

#define mr_alloc_rp(b, m) \
	(atomic64_add_return(b->mrs.memregs_block_size, \
			   &(m)->alloc_len) - \
			   b->mrs.memregs_block_size)
#define mr_free_rp(b, m) \
	(atomic64_sub_and_test(b->mrs.memregs_block_size, \
		    &(m)->alloc_len))

struct free_remote_page {
	struct list_head    list;
	struct remote_page       remote_page;
};

void __free_remote_page(struct emp_mm *, struct remote_page *);
bool alloc_remote_page(struct emp_mm *, struct emp_gpa *);
bool free_remote_page(struct emp_mm *, struct emp_gpa *, bool);
void remote_page_release(struct emp_mm *emm, struct emp_gpa *head, int num);
void adjust_remote_page_policy(struct emp_mm *emm, struct memreg *mr);
int remote_page_init(struct emp_mm *);
void remote_page_exit(struct emp_mm *);

#endif
