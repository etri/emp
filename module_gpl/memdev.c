#ifdef CONFIG_EMP_MEMDEV
#include <linux/version.h>
#include <linux/fs.h>
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE <= KERNEL_VERSION(5, 17, 0))
#include <linux/genhd.h>
#endif
#include <linux/nvme_ioctl.h>
#include <linux/uio.h>
#include <linux/sched/sysctl.h>
#include <linux/blk-mq.h>
#include "vm.h"
#include "udma.h"
#include "glue.h"
#include "debug.h"
#include "local_page.h"
#include "remote_page.h"

struct dma_ops memdev_dma_ops;

#if defined(CONFIG_EMP_SHOW_FAULT_PROGRESS) || defined(CONFIG_EMP_DEBUG)
static atomic64_t num_memdev_post_read = {0};
static atomic64_t num_memdev_post_write = {0};
#endif

static int memdev_create_local_page(struct local_page *lpage, int pglen, 
		struct context *context, int type)
{
	return 0;
}

static void memdev_destroy_local_page(struct local_page *lpage, int pglen,
		struct context *context, int type)
{
	return;
}

static inline void __emp_memdev_wait_read(struct emp_mm *bvma, struct work_request *w)
{
	// w->state was set at emp_memdev_post_read()
#ifdef CONFIG_EMP_STAT
	if (likely(bvma))
		atomic_inc(&bvma->stat.read_comp);
#endif
}

static int emp_memdev_wait_read(struct emp_mm *bvma, struct work_request *w,
			      struct vcpu_var *cpu, bool p)
{
	__emp_memdev_wait_read(bvma, w);
	return w->m_errors;
}

static int emp_memdev_try_wait_read(struct emp_mm *bvma, struct work_request *w,
				  struct vcpu_var *cpu, bool p)
{
	__emp_memdev_wait_read(bvma, w);
	return w->m_errors;
}

static int emp_memdev_wait_write(struct emp_mm *bvma, struct work_request *w,
			       struct vcpu_var *cpu, bool p)
{
	// w->state was set at emp_memdev_post_read()
#ifdef CONFIG_EMP_STAT
	if (likely(bvma))
		atomic_inc(&bvma->stat.write_comp);
#endif
	return w->m_errors;
}

static inline void __copy_pages(struct page *dst, struct page *src, int len)
{
	struct page *_dst, *_src;
	void *from, *to;
	int i;
	preempt_disable();
	pagefault_disable();
	for (i = 0, _dst = dst, _src = src; i < len; i++, _dst++, _src++) {
		from = page_address(_src);
		to = page_address(_dst);
		copy_page(to, from);
	}
	pagefault_enable();
	preempt_enable();
}

/**
 * emp_memdev_post_read - Request READ to memdev
 * @param conn connection
 * @param w work request
 * @param local_page the local page data structure for the data
 * @param pgoff page offset
 * @param pglen the number of page
 * @param criticality has priority or not
 * @param head_wr work request for the head of the page
 *
 * @return work request for the READ request
 */
static struct work_request *
emp_memdev_post_read(struct connection *conn, struct work_request *w,
		   struct local_page *local_page, unsigned long pgoff,
		   unsigned int pglen, int criticality,
		   struct work_request *head_wr, struct work_request **tail_wr)
{
#if defined(CONFIG_EMP_SHOW_FAULT_PROGRESS) || defined(CONFIG_EMP_DEBUG)
	s64 __num_memdev_post_read;
	__num_memdev_post_read = atomic64_inc_return(&num_memdev_post_read);
#endif
#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
	if (__num_memdev_post_read == 1 ||
			__num_memdev_post_read % CONFIG_EMP_SHOW_FAULT_PROGRESS_PERIOD == 0)
		printk(KERN_ERR "[EMP_PROGRESS] %s (memdev) post_read: %lld post_write: %lld "
				"emm: %d node: %d size: 0x%lx\n",
				__func__, __num_memdev_post_read,
				atomic64_read(&num_memdev_post_write),
				((struct emp_mm *)conn->emm)->id,
				conn->memdev_node, conn->memdev_size);
#endif
	atomic_set(&w->wr_refc, 0);
	INIT_LIST_HEAD(&w->sibling);
	w->state = STATE_POST_READ;
	w->context = conn->contexts[0];

	w->processor_id = smp_processor_id();
	w->head_wr = head_wr == NULL ? w : head_wr;
	atomic_inc(&w->head_wr->wr_refc);

	w->entangled_wr_len = 1;
	w->eh_wr = NULL;
	w->chained_ops = false;

	if (conn->contexts[0]->memdev_map) {
		struct page *page;
		unsigned long offset = pgoff / pglen;
		page = conn->contexts[0]->memdev_map[offset];
		if (unlikely(!page))
			memset(page_address(local_page->page), 0,
						pglen << PAGE_SHIFT);
		else
			__copy_pages(local_page->page, page, pglen);
	} else {
		void *page;
		void *memdev_ptr = conn->contexts[0]->memdev_ptr;
		debug_assert(memdev_ptr != NULL);
		page = (void *) (((char *)memdev_ptr) + (pgoff << PAGE_SHIFT));
		if (copy_from_user(page_address(local_page->page), page,
							pglen << PAGE_SHIFT)) {
			w->state = STATE_PR_FAILED;
			w->m_errors = -EFAULT;
			dprintk(KERN_ERR "[EMP_ERROR] %s (failed) mode: user error: %d post_read: %lld post_write: %lld "
					"emm: %d node: %d size: 0x%lx\n",
					__func__, w->m_errors,
					__num_memdev_post_read,
					atomic64_read(&num_memdev_post_write),
					((struct emp_mm *)conn->emm)->id,
					conn->memdev_node, conn->memdev_size);
			return w;
		}
	}

	w->state = STATE_READY;
	w->m_errors = 0;
	return w;
}

/**
 * emp_memdev_post_write - Request WRITE to memdev
 * @param conn connection
 * @param w work request
 * @param local_page the local page data structure for the data
 * @param pgoff page offset
 * @param pglen the number of page
 * @param head_wr work request for the head of the page
 * @param err error description
 *
 * @return work request for the WRITE request
 */
static struct work_request *
emp_memdev_post_write(struct connection *conn, struct work_request *w,
		struct local_page *local_page, unsigned long pgoff, int pglen,
		struct work_request *head_wr, struct work_request *tail_wr,
		int *err)
{
#if defined(CONFIG_EMP_SHOW_FAULT_PROGRESS) || defined(CONFIG_EMP_DEBUG)
	s64 __num_memdev_post_write;
	__num_memdev_post_write = atomic64_inc_return(&num_memdev_post_write);
#endif
#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
	if (__num_memdev_post_write == 1 ||
			__num_memdev_post_write % CONFIG_EMP_SHOW_FAULT_PROGRESS_PERIOD == 0)
		printk(KERN_ERR "[EMP_PROGRESS] %s (memdev) post_read: %lld post_write: %lld "
				"emm: %d node: %d size: 0x%lx\n",
				__func__, atomic64_read(&num_memdev_post_read),
				__num_memdev_post_write,
				((struct emp_mm *)conn->emm)->id,
				conn->memdev_node, conn->memdev_size);
#endif
	atomic_set(&w->wr_refc, 0);
	INIT_LIST_HEAD(&w->sibling);
	w->state = STATE_POST_WRITE;
	w->context = conn->contexts[0];

	w->processor_id = smp_processor_id();
	w->head_wr = head_wr == NULL ? w : head_wr;
	atomic_inc(&w->head_wr->wr_refc);

	w->entangled_wr_len = 1;
	w->eh_wr = NULL;
	w->chained_ops = false;

	if (conn->contexts[0]->memdev_map) {
		unsigned long offset;
		int sb_order;
		struct page *page;
		struct page **memdev_map = conn->contexts[0]->memdev_map;
		if (likely(w->gpa))
			sb_order = w->gpa->sb_order;
		else
			sb_order = bvma_subblock_order((struct emp_mm *) conn->emm);
		offset = pgoff >> sb_order;

		page = memdev_map[offset];
		if (!page) {
			page = emp_alloc_pages_node(conn->memdev_node, GFP_KERNEL, sb_order);
			if (unlikely(!page)) {
				/* TODO: we can failed early.
				 * post_write() can failed by set *err = error code
				 * and return NULL. However, we also need to consider
				 * other variables, e.g., context->wr_len.
				 * Instead, we just set w->state and w->m_errors to
				 * to indicate the error. wait_write() will fail.
				 */
				w->state = STATE_PW_FAILED;
				w->m_errors = -ENOMEM;
				dprintk(KERN_ERR "[EMP_ERROR] %s (failed) mode: kernel error: %d post_read: %lld post_write: %lld "
						"emm: %d node: %d size: 0x%lx\n",
						__func__, w->m_errors,
						atomic64_read(&num_memdev_post_read),
						__num_memdev_post_write,
						((struct emp_mm *)conn->emm)->id,
						conn->memdev_node, conn->memdev_size);
				return w;
			}

			memdev_map[offset] = page;
		}

		__copy_pages(page, local_page->page, pglen);
	} else {
		void *memdev_ptr = conn->contexts[0]->memdev_ptr;
		void *page;
		debug_assert(memdev_ptr != NULL);
		page = (void *) (((char *)memdev_ptr) + (pgoff << PAGE_SHIFT));
		if (copy_to_user(page, page_address(local_page->page),
							pglen << PAGE_SHIFT)) {
			w->state = STATE_PW_FAILED;
			w->m_errors = -EFAULT;
			dprintk(KERN_ERR "[EMP_ERROR] %s (failed) mode: user error: %d post_read: %lld post_write: %lld "
					"emm: %d node: %d size: 0x%lx\n",
					__func__, w->m_errors,
					atomic64_read(&num_memdev_post_read),
					__num_memdev_post_write,
					((struct emp_mm *)conn->emm)->id,
					conn->memdev_node, conn->memdev_size);
			return w;
		}
	}

	w->state = STATE_READY;
	w->m_errors = 0;
	return w;
}

/**
 * test_memdev - Test memory device
 * @param bdev block device
 * @param donor donor info
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int test_memdev(struct connection *conn)
{
	// TODO: implement the test code
	return 0;
}

/**
 * create_conn - Create connection to donor through memory device
 * @param emm emp_mm struct
 * @param connection connection info
 * @param donor donor info
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int create_conn(struct emp_mm *emm, struct connection **connection,
						struct donor_info *donor)
{
	int ret = 0;
	struct connection *conn;
	struct context *context;

	/* allocate a connection variable */
	conn = (struct connection *)emp_kzalloc(sizeof(struct connection), GFP_KERNEL);
	if (!conn) {
		ret = -ENOMEM;
		goto create_conn_error;
	}
	conn->contexts = emp_kzalloc(sizeof(struct context *), GFP_KERNEL);
	if (!conn->contexts) {
		emp_kfree(conn);
		ret = -ENOMEM;
		goto create_conn_error;
	}

	context = (struct context *)emp_kzalloc(sizeof(struct context), GFP_KERNEL);
	if (!context) {
		ret = -ENOMEM;
		goto create_context_error;
	}
	conn->contexts[0] = context;
	conn->n_contexts++;
	conn->memdev_node = donor->node;
	conn->memdev_size = donor->size;

	context->memdev_len = MB_TO_PAGE(conn->memdev_size) >> bvma_subblock_order(emm);
	dprintk(KERN_ERR "%s: memdev device. try to create. node: %d size: 0x%lx memdev_len: %ld type: %s\n",
				__func__, conn->memdev_node, conn->memdev_size, context->memdev_len,
				donor->ptr == NULL ? "kernel" : "user");
	if (donor->ptr == NULL) {
		context->memdev_ptr = NULL;
		context->memdev_map = (struct page **) emp_vzalloc(
				sizeof(struct page *) * context->memdev_len);
		if (!context->memdev_map) {
			ret = -ENOMEM;
			goto alloc_memdev_map_error;
		}
	} else {
		context->memdev_ptr = donor->ptr;
		context->memdev_map = NULL;
	}

	atomic_set(&context->is_connected, 1);
	context->ctrl_state = CONTEXT_READY;
	atomic_set(&conn->refcount, 1);

	*connection = conn;

	// following converts megabytes to page number
	atomic_set(&conn->contexts[0]->wr_len, 0);
	init_waitqueue_head(&conn->contexts[0]->wr_wq);
	
	dprintk(KERN_ERR "%s: memdev device. succeed to create. node: %d"
			" size: 0x%lx memdev_ptr: 0x%016lx memdev_map: 0x%016lx"
			" memdev_len: 0x%lx\n",
 				__func__, conn->memdev_node, conn->memdev_size,
				(unsigned long) conn->contexts[0]->memdev_ptr,
				(unsigned long) conn->contexts[0]->memdev_map,
				conn->contexts[0]->memdev_len);

	return ret;

alloc_memdev_map_error:
	emp_kfree(context);
create_context_error:
	emp_kfree(conn->contexts);
	emp_kfree(conn);
create_conn_error:
	return ret;
}

static int wait_for_conn(struct connection *conn)
{
	return 0;
}

/**
 * destroy_conn - Destroy connection to donor
 * @param connection connection info
 */
static int destroy_conn(struct emp_mm *emm, struct connection *conn)
{
	int ret = 0;
	struct memreg *mr = (struct memreg *)conn->memreg;
	struct context *context = conn->contexts[0];
	unsigned long i;
	unsigned int sb_order = bvma_subblock_order(emm);
	struct page *page;


	conn->contexts[0] = NULL;
	conn->n_contexts--;
	context->ctrl_state = CONTEXT_DESTROYING;
	wait_event_interruptible(context->wr_wq,
				atomic_read(&context->wr_len) == 0);
	atomic_dec(&conn->refcount);
	if (context->memdev_map) {
#ifdef CONFIG_EMP_DEBUG
		unsigned long allocated = 0;
		unsigned long memdev_len = context->memdev_len;
#endif
		for (i = 0; i < context->memdev_len; i++) {
			page = context->memdev_map[i];
			if (page == NULL)
				continue;
			emp_free_pages(page, sb_order);
			context->memdev_map[i] = NULL;
#ifdef CONFIG_EMP_DEBUG
			allocated++;
#endif
		}
		emp_vfree(context->memdev_map);
		dprintk(KERN_ERR "%s: memdev_map #allocated: %ld/%ld\n",
					__func__, allocated, memdev_len);
	}
	emp_kfree(context);
	emp_kfree(conn->contexts);
	emp_kfree(conn);

	mr->state = MR_STATE_CLOSING;
	mr->conn = NULL;

	return ret;
}

/**
 * memdev_init_wr - Initialize work request for memdev operation
 * @param w work request
 */
static void memdev_init_wr(struct work_request *w)
{
	init_completion(&w->m_wait);
}

/**
 * memdev_wr_ctor - Constructor of work request for memdev operation
 * @param opaque work request
 */
static void memdev_wr_ctor(void *opaque)
{
	struct work_request *w = (struct work_request *)opaque;
#ifdef CONFIG_EMP_RDMA
	init_waitqueue_head(&(w->wq));
#endif
	w->state = STATE_INIT;
	w->gpa = NULL;
	INIT_LIST_HEAD(&w->sibling);
	INIT_LIST_HEAD(&w->subsibling);
	INIT_LIST_HEAD(&w->remote_pages);

	init_completion(&w->m_wait);
	atomic_set(&w->wr_refc, 0);
	w->dma_ops = &memdev_dma_ops;
}

int memdev_open(void *opaque)
{
	return 0;
}

void memdev_release(void *opaque)
{
	return;
}

/**
 * memdev_init - Initialize Memory DeviceNVMe
 *
 * @retval 0: Success
 */
int memdev_init(void)
{
	memdev_dma_ops.create_conn = create_conn;
	memdev_dma_ops.wait_for_conn = wait_for_conn;
	memdev_dma_ops.destroy_conn = destroy_conn;

	memdev_dma_ops.wait_read = emp_memdev_wait_read;
	memdev_dma_ops.try_wait_read = emp_memdev_try_wait_read;
	memdev_dma_ops.wait_write = emp_memdev_wait_write;
	memdev_dma_ops.post_read = emp_memdev_post_read;
	memdev_dma_ops.post_write = emp_memdev_post_write;

	memdev_dma_ops.setup_local_page = memdev_create_local_page;
	memdev_dma_ops.destroy_local_page = memdev_destroy_local_page;
	memdev_dma_ops.wr_ctor = memdev_wr_ctor;
	memdev_dma_ops.init_wr = memdev_init_wr;

	memdev_dma_ops.test_conn = test_memdev;

	return 0;
}

void memdev_exit(void)
{
	return;
}
#endif /* CONFIG_EMP_MEMDEV */
