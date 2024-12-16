#ifndef __DMA_H__
#define __DMA_H__

#include <linux/slab.h>
#include <linux/blkdev.h>
#include <scsi/scsi_cmnd.h>
#include <rdma/rdma_cm.h>
#include <linux/nvme.h>
#include "config.h"
#include "emp_type.h"
#include "vm.h"
#include "remote_page.h"
#include "../include/emp_ioctl.h"
#include "debug.h"

enum dma_type {
	DMA_READ,
	DMA_WRITE,
	DMA_UNKNOWN,
};

struct work_request {
	// sibling is for a link to wb_request_list
	struct list_head            sibling;
	// subsibling constructs a list for the members in a block
	struct list_head            subsibling;
	struct emp_gpa       *gpa;
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	struct local_page *local_page;
#endif
#ifdef CONFIG_EMP_DEBUG_INTEGRITY
	// intermediate value for partial data decryption
	void *addr_upto;
	u64 tag_upto;
#endif

	enum wr_type {
		TYPE_FETCHING,
		TYPE_WB,
		TYPE_RCW,
		TYPE_FETCHING_MEMPOLL,
	} type;

	struct dma_ops *dma_ops;

	/* wr_refc is for counting the numbers of work request
	 * referencing this work request.
	 *
	 * For example, if a block is made of n sub-blocks and
	 * the block is currently fetched, there should be n sub work requests.
	 * To save the reference count, we are using wr_refc.  */
	atomic_t                    wr_refc;
	int                         wr_size;

	volatile enum wr_state {
		STATE_INIT,
		STATE_READY,
		STATE_POST_READ,
		STATE_POST_WRITE,
		STATE_SEND,
		STATE_PR_FAILED,
		STATE_PW_FAILED,
	} state;

	unsigned int                processor_id;
	void                        *context;

	union {
#ifdef CONFIG_EMP_BLOCKDEV
		struct {
			struct completion       wait;
			struct bio      *bio;
			int             command;
			blk_qc_t        cookie;
			struct request_queue    *q;
			int                     errors;
		};
#endif
	};

	struct work_request         *head_wr;
	struct work_request         *eh_wr;
	int                         entangled_wr_len;

	struct remote_page          remote_page;
	struct list_head            remote_pages;
	bool                        chained_ops;
#ifdef CONFIG_EMP_DEBUG_PROGRESS
	struct progress_info progress;
#endif
} __attribute__((aligned(8)));

struct context {
	struct connection *conn;
	atomic_t            is_connected;
	enum {
		CONTEXT_CONNECTING,
		CONTEXT_READY,
		CONTEXT_INVL_SIZE,
		CONTEXT_DESTROYING,
	} ctrl_state;
	wait_queue_head_t   ctrl_wq;

	union {
#ifdef CONFIG_EMP_BLOCKDEV
		/* Block Device */
		struct {
			u64                 base;
		};
#endif
	};

	atomic_t          wr_len; /* # related work requests */
	wait_queue_head_t wr_wq;
	int               id;

};

struct connection {
	atomic_t        refcount;
	wait_queue_head_t ctrl_wq;
	union {
#ifdef CONFIG_EMP_BLOCKDEV
		/* Block Device */
		struct {
			struct block_device *bdev;
			unsigned int         base;
			size_t               size;
		};
#endif
	};

	void    *emm;
	union {
		struct {
			int mrid;
			int mode;
		};
		void        *memreg;
	};
	unsigned int        read_command_flag;
	unsigned int        read_prefetch_flag;
	unsigned int        write_command_flag;

	int                 n_contexts;
	struct context      **contexts;
	int                 write_criticality;
	u32                 latency;
};

struct dma_ops {
	int (*create_conn)(struct emp_mm *, struct connection **, struct donor_info *);
	int (*wait_for_conn)(struct connection *);
	int (*destroy_conn)(struct emp_mm *, struct connection *);
	int (*wait_read)(struct emp_mm *, struct work_request *,
			struct vcpu_var *, bool);
	int (*try_wait_read)(struct emp_mm *, struct work_request *, 
			     struct vcpu_var *, bool);
	int (*wait_write)(struct emp_mm *, struct work_request *,
			  struct vcpu_var *, bool);
	struct work_request *(*post_read)(struct connection *,
					  struct work_request *,
					  struct local_page *, unsigned long,
					  unsigned int, int, 
					  struct work_request *,
					  struct work_request **);
	struct work_request *(*post_write)(struct connection *,
					   struct work_request *, 
					   struct local_page *, unsigned long,
					   int, struct work_request *, 
					   struct work_request *, int *);
	bool (*post_work_req)(struct work_request *, struct work_request *,
			      struct work_request *, int);

	int (*setup_local_page)(struct local_page *, int, struct context *, int);
	void (*destroy_local_page)(struct local_page *, int, struct context *, int);

	void (*wr_ctor)(void *);
	void (*init_wr)(struct work_request *);

	int (*test_conn)(struct connection *);
};

int dma_open(void *);
void dma_release(void *);
int dma_init(void);
void dma_exit(void);

#define WR_READY(w)     (READ_ONCE((w)->state) == STATE_READY)
#define WR_PR_FAILED(w) (READ_ONCE((w)->state) == STATE_PR_FAILED)
#define WR_PW_FAILED(w) (READ_ONCE((w)->state) == STATE_PW_FAILED)

#define CONN_GET_CONTEXT(conn, criticality) \
	(((criticality) < (conn)->n_contexts) ? ((conn)->contexts[criticality]) \
					      : ((conn)->contexts[0]))

#define WR_SGE_LEN(w) (0)

#ifdef CONFIG_EMP_BLOCKDEV
extern struct dma_ops nvme_dma_ops;
#endif

static inline int get_wr_remote_page_mrid(struct work_request *w) {
	debug_assert(is_remote_page_cow(&w->remote_page) == false);
	return __get_remote_page_mrid(&w->remote_page);
}

static inline bool is_wr_remote_page_freed(struct work_request *w) {
	return is_remote_page_freed(&w->remote_page);
}

static inline void set_wr_remote_page_freed(struct work_request *w) {
	set_remote_page_freed(&w->remote_page);
}

static inline void init_wr_remote_page(struct work_request *w, u64 val)
{
	debug_assert((val & REMOTE_PAGE_FLAG_MASK) == 0);
	__set_remote_page(&w->remote_page, val);
}

static inline int conn_get_mr_id(struct connection *conn) {
	if (conn == NULL || conn->memreg == NULL)
		return -1;
	return ((struct memreg *)conn->memreg)->id;
}

static inline int conn_get_ctrl_state(struct connection *conn) {
	return 0;
}

/**
 * alloc_work_request - Allocate a work request to DMA
 * @param b bvma data structure
 * @param m memory region
 *
 * @return allocated work request
 */
static inline struct work_request *
alloc_work_request(struct memreg *m, struct dma_ops *ops)
{
	struct work_request *w;

	atomic_inc(&m->wr_len);
	w = emp_kmem_cache_alloc(m->wr_cache, GFP_ATOMIC);
	if (unlikely(!w)) {
		if (atomic_dec_and_test(&m->wr_len) &&
				(m->state == MR_STATE_CLOSING))
			wake_up_interruptible(&m->wr_wq);
		return NULL;
	}
	init_progress_info(w);
	debug_progress_start(w, 0);

#ifdef CONFIG_EMP_DEBUG_INTEGRITY
	w->addr_upto = NULL;
	w->tag_upto = 0;
#endif
	w->dma_ops = ops;
	ops->init_wr(w);

	return w;
}

/**
 * free_work_request - Free a work request to DMA
 * @param b bvma data structure
 * @param w work request to free
 */
static inline void 
free_work_request(struct emp_mm *b, struct work_request *w)
{
	int mid = get_wr_remote_page_mrid(w);
	struct memreg *m = b->mrs.memregs[mid];

	emp_kmem_cache_free(m->wr_cache, w);
	if (atomic_dec_and_test(&m->wr_len) &&
			(m->state == MR_STATE_CLOSING))
		wake_up_interruptible(&m->wr_wq);
}

/**
 * post_read - Post a READ request to NVMe/RDMA
 * @param conn connection info
 * @param gpa gpa for data
 * @param pgoff_rba page offset
 * @param pgoff_len size of data
 * @param rp remote page descriptor
 * @param criticality priority
 * @param head_wr work request for head
 * @param ops DMA operation info
 *
 * @return work request for the READ operation
 *
 * Allocate a work request according to a READ request
 * and post the READ request
 */
static inline struct work_request *
post_read(struct connection *conn, struct emp_gpa *gpa, unsigned long pgoff_rba,
	  unsigned int pgoff_len, unsigned long long rp, int criticality,
	  struct work_request *head_wr, struct work_request **tail_wr,
	  struct dma_ops *ops)
{
	struct work_request *w;
	struct local_page *local_page = gpa->local_page;
	struct memreg *mr = (struct memreg *)conn->memreg;
	struct context *context = CONN_GET_CONTEXT(conn, criticality);

	if (!context || context->ctrl_state == CONTEXT_DESTROYING
		|| !mr || mr->state == MR_STATE_CLOSING) {
		return ERR_PTR(-ENXIO);
	}

	w = alloc_work_request(mr, ops);
	if (!w) {
		printk(KERN_ERR "failed to allocate work request\n");
		return ERR_PTR(-ENOMEM);
	}
	w->gpa = gpa;
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	w->local_page = gpa->local_page;
#endif
	init_wr_remote_page(w, rp);
	w->type = TYPE_FETCHING;

	atomic_inc(&context->wr_len);
	/* Map DMA page */
	ops->setup_local_page(local_page, pgoff_len, context, DMA_READ);

	w = ops->post_read(conn, w, local_page, pgoff_rba, pgoff_len,
			      criticality, head_wr, tail_wr);
	return w;
}

/**
 * post_write - Post a WRITE request to NVMe/RDMA
 * @param conn connection info
 * @param gpa gpa for data
 * @param pgoff page offset
 * @param pglen size of pagesa
 * @param rp remote page descriptor
 * @param err error description
 * @param ops DMA operation info
 *
 * @return work request for the WRITE operation
 *
 * Allocate a work request according to a WRITE request
 * and post the WRITE request
 */
static inline struct work_request *
post_write(struct connection *conn, struct emp_gpa *gpa,
	   unsigned long pgoff, int pglen, 
	   struct work_request *head_wr, struct work_request *tail_wr,
	   unsigned long long rp, int *err, struct dma_ops *ops)
{
	struct work_request *w;
	struct local_page *local_page = gpa->local_page;
	struct memreg *mr = (struct memreg *)conn->memreg;
	struct context *context = CONN_GET_CONTEXT(conn, conn->write_criticality);

	if (!context || context->ctrl_state == CONTEXT_DESTROYING
		|| !mr || mr->state == MR_STATE_CLOSING) {
		*err = -ENXIO;
		return NULL;
	}

	w = alloc_work_request(mr, ops);
	if (!w) {
		printk(KERN_ERR "%s:%d: failed to allocate work request\n",
				__func__, __LINE__);
		*err = -ENOMEM;
		return NULL;
	}
	w->gpa = gpa;
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	w->local_page = gpa->local_page;
#endif
	init_wr_remote_page(w, rp);
	w->type = TYPE_WB;

	atomic_inc(&context->wr_len);
	/* Map DMA page */
	ops->setup_local_page(local_page, pglen, context, DMA_WRITE);

	w = ops->post_write(conn, w, local_page, pgoff, pglen,
			       head_wr, tail_wr, err);
	return w;
}

/**
 * wait_read - Wait for the completion of the READ request
 * @param bvma bvma data structure
 * @param w work request for waiting
 * @param vcpu working vcpu ID
 * @param prefetch the request is for prefetch?
 *
 * @return error description
 */
static inline int wait_read(struct emp_mm *bvma, struct work_request *w,
			    struct vcpu_var *cpu, bool prefetch)
{
	int ret;
	struct context *context = (struct context *)w->context;
	struct dma_ops *ops = (struct dma_ops *)w->dma_ops;
	ret = ops->wait_read(bvma, w, cpu, prefetch);

	/* Unmap DMA page */
	ops->destroy_local_page(w->gpa->local_page, WR_SGE_LEN(w), context, DMA_READ);
	if (atomic_dec_and_test(&context->wr_len) &&
			(context->ctrl_state == CONTEXT_DESTROYING))
		wake_up_interruptible(&context->wr_wq);

	return ret;
}

/**
 * try_wait_read - Check for the completion of the READ request
 * @param bvma bvma data structure
 * @param w work request for waiting
 * @param vcpu working vcpu ID
 * @param prefetch the request is for prefetch?
 *
 * @return error description
 */
static inline int try_wait_read(struct emp_mm *bvma, struct work_request *w,
				struct vcpu_var *cpu, bool prefetch)
{
	int ret;
	struct context* context = (struct context *)w->context;
	struct dma_ops *ops = (struct dma_ops *)w->dma_ops;
	ret = ops->try_wait_read(bvma, w, cpu, prefetch);

	if (ret == 1) {
		/* If wait completion, unmap DMA page */
		ops->destroy_local_page(w->gpa->local_page, WR_SGE_LEN(w), context, DMA_READ);
		if (atomic_dec_and_test(&context->wr_len) &&
				(context->ctrl_state == CONTEXT_DESTROYING))
			wake_up_interruptible(&context->wr_wq);
	}

	return ret;
}

/**
 * wait_write - Wait for the completion of the WRITE request
 * @param bvma bvma data structure
 * @param w work request for waiting
 * @param vcpu working vcpu ID
 * @param prefetch the request is for prefetch?
 *
 * @return error description
 */
static inline int wait_write(struct emp_mm *bvma, struct work_request *w,
			     struct vcpu_var *cpu, bool prefetch)
{
	int ret;
	struct context* context = (struct context *)w->context;
	struct dma_ops *ops = (struct dma_ops *)w->dma_ops;
	int eh_wr_len = w->head_wr->entangled_wr_len;
	ret = (*ops->wait_write)(bvma, w, cpu, prefetch);

	/* Unmap DMA page */
	ops->destroy_local_page(w->gpa->local_page, WR_SGE_LEN(w), context, DMA_WRITE);
	eh_wr_len = atomic_sub_return(eh_wr_len, &context->wr_len);
	if (eh_wr_len == 0 &&
			(context->ctrl_state == CONTEXT_DESTROYING))
		wake_up_interruptible(&context->wr_wq);

	return ret;
}

static inline void destroy_write(struct emp_mm *bvma, struct work_request *w)
{
	struct context* context = (struct context *)w->context;
	struct dma_ops *ops = (struct dma_ops *)w->dma_ops;
	int eh_wr_len = w->head_wr->entangled_wr_len;

	/* Unmap DMA page */
	ops->destroy_local_page(w->gpa->local_page, WR_SGE_LEN(w), context, DMA_WRITE);
	eh_wr_len = atomic_sub_return(eh_wr_len, &context->wr_len);
	if (eh_wr_len == 0 &&
			(context->ctrl_state == CONTEXT_DESTROYING))
		wake_up_interruptible(&context->wr_wq);
}
#endif /* __DMA_H__ */
