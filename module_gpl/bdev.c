#ifdef CONFIG_EMP_BLOCKDEV
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <linux/uio.h>
#include <linux/sched/sysctl.h>
#include <linux/blk-mq.h>
#include "vm.h"
#ifdef CONFIG_EMP_BLOCKDEV
#include "nvme.h"
#endif
#include "udma.h"
#include "glue.h"
#include "debug.h"
#include "local_page.h"
#include "remote_page.h"

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0))
#include <linux/genhd.h>
#endif

static int wait_for_wc(struct emp_mm *, struct vcpu_var *, struct work_request *);
static int try_wait_for_wc(struct emp_mm *, struct work_request *);

#define __PAGE_TO_BLOCK(x, s)   ((x) << (PAGE_SHIFT - (s)))

// assuem default block size as 512
#define PAGE_TO_BLOCK(x)        __PAGE_TO_BLOCK(x, 9)

#define READ_FROM_BLOCK 0
#define WRITE_TO_BLOCK  1

#define get_wr_context(w) ((struct context *) (w)->context)

struct dma_ops nvme_dma_ops;

/**
 * write_deadbeef_page - Initialize empty page
 * @param page the page to initialize
 * @param page_order the number of pages
 */
static void write_deadbeef_page(unsigned long page, int page_order)
{
	int i;
	int *target_page = (int *)page;

	for (i = 0; i < ((PAGE_SIZE << page_order)/sizeof(int)); i++)
		*(target_page + i) = 0xdeadbeef;

	return;
}

/**
 * compare_page - Compare a page with the initialized page to check the correctness
 * @param page the page to compare
 * @param page_order the number of pages
 * @param devpath the path of device
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int compare_page(unsigned long page, int page_order, char *devpath)
{
	int total, invalid;
	int i;
	int *target_page = (int *)page;

	for (i = 0, total = 0, invalid = 0; 
			i < ((PAGE_SIZE << page_order)/sizeof(int)); i++) {
		total++;
		if (*(target_page + i)  != 0xdeadbeef)
			invalid++;
	}
	printk("%s %s total: %d, invalid: %d\n",
			devpath, __func__, total, invalid);

	return invalid? 1: 0;
}

static int blk_create_local_page(struct local_page *lpage, int pglen, 
		struct context *context, int type)
{
	return 0;
}

static void blk_destroy_local_page(struct local_page *lpage, int pglen,
		struct context *context, int type)
{
	return;
}

/**
 * bdev_end_io_read_page - A callback function when the read Block I/O is completed
 * @param bio Block I/O data structure
 */
static void bdev_end_io_read_page(struct bio *bio) {
	int error = blk_status_to_errno(bio->bi_status);
	struct work_request *w = bio->bi_private;

	if (unlikely(!w || !w->bio || w->bio != bio)) {
		printk(KERN_ERR "WARN: [%s] missing work request "
				"w: %016lx w->bio: %016lx bio: %016lx\n",
					__func__,
					(unsigned long) w,
					w ? (unsigned long) w->bio : 0UL,
					(unsigned long) bio);
		bio_put(bio);
		return;
	}

	w->errors = error;
	w->bio = (void *)0xdeadbeef;
	wmb();
	complete(&w->wait);
	bio_put(bio);
}

/**
 * bdev_end_io_write_page - A callback function when the write Block I/O is completed
 * @param bio Block I/O data structure
 */
static void bdev_end_io_write_page(struct bio *bio) {
	int error = blk_status_to_errno(bio->bi_status);
	struct work_request *w = bio->bi_private;

	if (unlikely(!w || !w->bio || w->bio != bio)) {
		printk(KERN_ERR "WARN: [%s] missing work request "
				"w: %016lx w->bio: %016lx bio: %016lx\n",
					__func__,
					(unsigned long) w,
					w ? (unsigned long) w->bio : 0UL,
					(unsigned long) bio);
		bio_put(bio);
		return;
	}

	w->errors = error;
	w->bio = (void *)0xdeadbeef;
	wmb();
	complete(&w->wait);
	bio_put(bio);
}

/**
 * emp_bdev_wait_rw - A Block I/O waiting function for read or write operations
 * @param bvma bvma data structure
 * @param w work request
 * @param c working CPU ID
 * @param p prefetch request?
 *
 * @return description when the error occurs
 */
static int emp_bdev_wait_rw(struct emp_mm *bvma, struct work_request *w,
			    struct vcpu_var *cpu, bool p, int rw)
{
	debug_emp_bdev_wait_rw(w, rw);

	if (wait_for_wc(bvma, cpu, w))
		return -1;

	w->state = STATE_READY;
	return w->errors;
}

/**
 * emp_bdev_try_wait_rw - A Block I/O check completion function for read operations
 * @param bvma bvma data structure
 * @param w work request
 *
 * @retval -1: Failed to check completion
 * @return description when the error occurs
 */
static int emp_bdev_try_wait_rw(struct emp_mm *bvma, struct work_request *w, int rw)
{
	debug_emp_bdev_wait_rw(w, rw);

	if (!try_wait_for_wc(bvma, w))
		return -1;

	w->state = STATE_READY;
	return w->errors;
}

static int emp_bdev_wait_read(struct emp_mm *bvma, struct work_request *w,
			      struct vcpu_var *cpu, bool p)
{
	return emp_bdev_wait_rw(bvma, w, cpu, p, READ_FROM_BLOCK);
}

static int emp_bdev_try_wait_read(struct emp_mm *bvma, struct work_request *w,
				  struct vcpu_var *cpu, bool p)
{
	return emp_bdev_try_wait_rw(bvma, w, READ_FROM_BLOCK);
}

static int emp_bdev_wait_write(struct emp_mm *bvma, struct work_request *w,
			       struct vcpu_var *cpu, bool p)
{
	return emp_bdev_wait_rw(bvma, w, cpu, p, WRITE_TO_BLOCK);
}

/**
 * emp_bdev_rw_page - A Block I/O request submit function (read/write pages)
 * @param bdev Block device
 * @param sector sector
 * @param size size of the data for I/O
 * @param page the page which contains data
 * @param w work request
 * @param rw read or write
 * @param command_flag Block I/O command flag
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int emp_bdev_rw_page(struct block_device *bdev, sector_t sector,
		int size, struct page *page, struct work_request *w, int rw,
		unsigned int command_flag)
{
	struct bio *bio;
	int command;

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0))
	bio = bio_alloc(GFP_NOIO, 1);
#else
	bio = bio_alloc(bdev, 1, REQ_SYNC, GFP_NOIO);
#endif
	if (unlikely(bio == NULL)) {
		w->bio = NULL;
		return -1;
	}
	
#if defined(bio_set_dev)
	bio_set_dev(bio, bdev);
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0))
	w->q = bdev->bd_disk->queue;
#else
	w->q = bdev->bd_queue;
#endif
#else
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0))
	bio->bi_disk = bdev->bd_disk;
	w->q = bdev->bd_disk->queue;
#else
	bio->bi_bdev= bdev;
	w->q = bdev->bd_queue;
#endif
#endif
	bio->bi_iter.bi_sector = sector + get_start_sect(bdev);
	if (bio_add_page(bio, page, size, 0) == 0)
		goto error;
	
	bio->bi_private = w;
	if (rw == READ_FROM_BLOCK) {
		bio->bi_end_io = bdev_end_io_read_page;
		w->type = TYPE_FETCHING;
		command = READ | REQ_SYNC;
		command |= command_flag;
	} else {
		bio->bi_end_io = bdev_end_io_write_page;
		w->type = TYPE_WB;
		command = WRITE | REQ_SYNC;
		command |= command_flag;
		/*command = WRITE_FLUSH;*/
	}
	w->bio = bio;

	bio->bi_opf = command;
	w->command = command;

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
	w->cookie = submit_bio(bio);
	if ((command & REQ_HIPRI) && !blk_qc_t_valid(w->cookie))
		goto error;
#else
	submit_bio(bio);
	w->cookie = READ_ONCE(bio->bi_cookie);
#endif

	/* NOTE: submit_bio() does not ensure the I/O is scheduled.
	 *       We should call io_schedule(). However, it should be
	 *       called without any locks. Thus, we call io_schedule()
	 *       in other places.
	 */

	return 0;

error:
	bio->bi_private = NULL;
	w->bio = NULL;
	bio_put(bio);
	return -1;
}

/**
 * emp_bdev_post_read - Request READ to Block I/O device
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
emp_bdev_post_read(struct connection *conn, struct work_request *w,
		   struct local_page *local_page, unsigned long pgoff,
		   unsigned int pglen, int criticality,
		   struct work_request *head_wr, struct work_request **tail_wr)
{
	int ret;
	unsigned int command_flag;
	struct request_queue *q;

	atomic_set(&w->wr_refc, 0);
	INIT_LIST_HEAD(&w->sibling);
	w->state = STATE_POST_READ;
	w->context = conn->contexts[0];

	w->processor_id = emp_smp_processor_id();
	w->head_wr = head_wr == NULL ? w : head_wr;
	atomic_inc(&w->head_wr->wr_refc);

	q = conn->bdev->bd_disk->queue;

	/* TODO: read_command_flag is set to REQ_HIPRI as default
	 * 	 should make default flag 0 */
	command_flag = criticality?
		conn->read_command_flag: conn->read_prefetch_flag;
	/* read remote pages at remote_base with remote_size to local pages */
	w->entangled_wr_len = 1;
	w->eh_wr = NULL;
	w->chained_ops = false;
	do {
		ret = emp_bdev_rw_page(conn->bdev, 
			PAGE_TO_BLOCK(pgoff + get_wr_context(w)->base),
			pglen << PAGE_SHIFT,
			local_page->page, w, READ_FROM_BLOCK,
			command_flag);
		if (ret < 0)
			cond_resched();
	} while (ret < 0);

	return w;
}

/**
 * emp_bdev_post_write - Request WRITE to Block I/O device
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
emp_bdev_post_write(struct connection *conn, struct work_request *w,
		struct local_page *local_page, unsigned long pgoff, int pglen,
		struct work_request *head_wr, struct work_request *tail_wr,
		int *err)
{
	int ret;

	atomic_set(&w->wr_refc, 0);
	INIT_LIST_HEAD(&w->sibling);
	w->state = STATE_POST_WRITE;
	w->context = conn->contexts[0];

	w->processor_id = emp_smp_processor_id();
	w->head_wr = head_wr == NULL ? w : head_wr;
	atomic_inc(&w->head_wr->wr_refc);

	w->entangled_wr_len = 1;
	w->eh_wr = NULL;
	w->chained_ops = false;
	/* write local pages to remote_base with remote_size to remote pages */
	do {
		ret = emp_bdev_rw_page(conn->bdev,
			PAGE_TO_BLOCK(pgoff + get_wr_context(w)->base),
			pglen << PAGE_SHIFT,
			local_page->page, w, WRITE_TO_BLOCK,
			conn->write_command_flag);
		if (ret < 0)
			cond_resched();
	} while (ret < 0);

	return w;
}

/**
 * test_bdev - Test Block I/O device
 * @param bdev block device
 * @param donor donor info
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int test_bdev(struct connection *conn)
{
	int ret = 0, i;
	u64 *write_addr, *read_addr;
	struct page *write_pg, *read_pg;
	struct work_request w;
	sector_t base[2];

	write_pg = emp_alloc_page(GFP_KERNEL);
	if (write_pg == NULL) {
		ret = -1;
		goto out;
	}

	read_pg = emp_alloc_page(GFP_KERNEL);
	if (read_pg == NULL) {
		ret = -1;
		goto free_write_pg_out;
	}

	write_addr = page_address(write_pg);
	read_addr = page_address(read_pg);

	base[0] = PAGE_TO_BLOCK(MB_TO_PAGE(conn->base));
	base[1] = MB_TO_PAGE(conn->base) + MB_TO_PAGE(conn->size);
	base[1] = PAGE_TO_BLOCK(base[1] - 1);

	for (i = 0; i < 2; i++) {
		// initialize the pages
		write_deadbeef_page((unsigned long)write_addr, 0);
		memset(read_addr, 0, PAGE_SIZE);

		memset(&w, 0, sizeof(w));
		init_completion(&w.wait);
		ret = emp_bdev_rw_page(conn->bdev, base[i], PAGE_SIZE, write_pg, &w,
				WRITE_TO_BLOCK, 0);
		if (ret)
			break;
		ret = emp_bdev_wait_rw(NULL, &w, 0, false, WRITE_TO_BLOCK);
		if (ret)
			break;

		memset(&w, 0, sizeof(w));
		init_completion(&w.wait);
		ret = emp_bdev_rw_page(conn->bdev, base[i], PAGE_SIZE, read_pg, &w,
				READ_FROM_BLOCK, 0);
		if (ret)
			break;
		ret = emp_bdev_wait_rw(NULL, &w, 0, false, READ_FROM_BLOCK);
		if (ret)
			break;

		printk("check the page on block %llx\n", (u64)base[i]);
		ret = compare_page((unsigned long)page_address(read_pg), 0,
				conn->bdev->bd_disk->disk_name);
		if (ret)
			break;
	}

	emp_free_page(read_pg);
free_write_pg_out:
	emp_free_page(write_pg);
out:
	return ret;
}

/**
 * create_conn - Create connection to donor through Block I/O device
 * @parem emm emp_mm struct
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
	struct block_device *bdev;
	struct nvme_ns *ns;

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

	/* open a block device by path */
	bdev = blkdev_get_by_path(donor->path, O_RDONLY, NULL);
	if(IS_ERR(bdev)) {
		printk(KERN_ERR "failed to open block device\n");
		ret = -EINVAL;
		goto open_blkdev_error;
	}

	/* retrieve nvme namespace via pointer accesses */
	if (!bdev->bd_disk) {
		printk(KERN_ERR "bdev->bd_disk == NULL\n");
		ret = -EINVAL;
		goto open_blkdev_error;
	}

	ns = bdev->bd_disk->private_data;
	if (!ns) {
		printk(KERN_ERR "bdev->bd_disk->private_data == NULL\n");
		ret = -EINVAL;
		goto open_blkdev_error;
	}

	conn->base = donor->base;
	conn->size = donor->size;
	conn->bdev = bdev;
	/* test and set member variables of connection */
	if (test_bdev(conn)) {
		printk(KERN_ERR "checking %s failed\n", donor->path);
		ret = -EINVAL;
		goto open_blkdev_error;
	}

	atomic_set(&context->is_connected, 1);
	context->ctrl_state = CONTEXT_READY;
	atomic_set(&conn->refcount, 1);

	conn->write_criticality = 0;
	conn->read_command_flag = 0;
	conn->write_command_flag = 0;
	conn->read_prefetch_flag = 0;

	{
		struct request_queue *q = bdev->bd_disk->queue;
		if (test_bit(QUEUE_FLAG_POLL, &q->queue_flags)) {
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
			conn->read_command_flag = REQ_HIPRI;
#else
			conn->read_command_flag = REQ_POLLED;
#endif
		}
	}

	*connection = conn;

	// following converts megabytes to page number
	conn->contexts[0]->base = MB_TO_PAGE((u64)donor->base);
	atomic_set(&conn->contexts[0]->wr_len, 0);
	init_waitqueue_head(&conn->contexts[0]->wr_wq);

	return ret;

open_blkdev_error:
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

	conn->contexts[0] = NULL;
	conn->n_contexts--;
	context->ctrl_state = CONTEXT_DESTROYING;
	wait_event_interruptible(context->wr_wq,
				atomic_read(&context->wr_len) == 0);
	atomic_dec(&conn->refcount);
	emp_kfree(context);
	emp_kfree(conn->contexts);
	emp_kfree(conn);

	mr->state = MR_STATE_CLOSING;
	mr->conn = NULL;

	return ret;
}

void check_and_writeback_inactive_block(struct emp_mm *, int);

/**
 * wait_for_wc - Wait for the work request completion
 * @param bvma bvma data structure
 * @param cpu working CPU ID
 * @param w work request
 *
 * @return description when the error occurs
 *
 * Wait the work completion according to the command flag
 * + HIPRI (High priority) requests: Polling the completion
 * + other requests: Interrupt-based handling
 */
static int 
wait_for_wc(struct emp_mm *bvma, struct vcpu_var *cpu, struct work_request *w)
{
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
	if ((w->command & REQ_HIPRI) &&
			test_bit(QUEUE_FLAG_POLL, &w->q->queue_flags)) {
#else
	if ((w->command & REQ_POLLED) &&
			test_bit(QUEUE_FLAG_POLL, &w->q->queue_flags)) {
#endif
		// although target work request is completed, we should call
		// blk_poll to prevent from being stucked on completion queue
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
		blk_poll(w->q, w->cookie, false);
#else
		bio_poll(w->bio, NULL, false);
#endif
		while (!completion_done(&w->wait)) {
			/*if (bvma && w->type == TYPE_FETCHING) {*/
				/*check_and_writeback_inactive_block(bvma, cpu);*/
				/*[>blk_poll(w->q, w->cookie, false);<]*/
				/*[>cond_resched();<]*/
				/*[>continue;<]*/
			/*}*/
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
			blk_poll(w->q, w->cookie, true);
#else
			bio_poll(w->bio, NULL, false);
#endif

			cond_resched();
		}
	} else {
		unsigned long hang_check;

		completion_done(&w->wait);

		// w->req is handled by the interrupt handler (dma_end_sync_rq)
		// the existence of w->req means that there have not been interrupted on
		// the related completion queue and the vcpu context may wait for long time

		/* Prevent hang_check timer from firing at us during very long I/O */
		hang_check = kernel_sysctl_hung_task_timeout_secs();
		if (hang_check) {
			while (!wait_for_completion_io_timeout(&w->wait,
						hang_check * (HZ/2)))
				;
		} else {
			wait_for_completion_io(&w->wait);
		}
	}
	return w->errors;
}

/**
 * try_wait_for_wc - Wait for the work request completion
 * @param bvma bvma data structure
 * @param cpu working CPU ID
 * @param w work request
 *
 * @retval 1: Succeed to check the completion of the work req.
 * @retval 0: failed to check the completion of the work req.
 */
static int try_wait_for_wc(struct emp_mm *bvma, struct work_request *w)
{
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
	if ((w->command & REQ_HIPRI) &&
			test_bit(QUEUE_FLAG_POLL, &w->q->queue_flags)) {
		blk_poll(w->q, w->cookie, false);
	}
#else
	if ((w->command & REQ_POLLED) &&
			test_bit(QUEUE_FLAG_POLL, &w->q->queue_flags)) {
		bio_poll(w->bio, NULL, false);
	}
#endif
	return completion_done(&w->wait) ? 1: 0;
}

/**
 * nvme_init_wr - Initialize work request for NVMe operation
 * @param w work request
 */
static void nvme_init_wr(struct work_request *w)
{
	init_completion(&w->wait);
	w->bio = NULL;
	w->command = 0;
	w->cookie = BLK_QC_T_NONE;
}

/**
 * nvme_wr_ctor - Constructor of work request for NVMe operation
 * @param opaque work request
 */
static void nvme_wr_ctor(void *opaque)
{
	struct work_request *w = (struct work_request *)opaque;
	w->state = STATE_INIT;
	w->gpa = NULL;
	INIT_LIST_HEAD(&w->sibling);
	INIT_LIST_HEAD(&w->subsibling);
	INIT_LIST_HEAD(&w->remote_pages);

	init_completion(&w->wait);
	atomic_set(&w->wr_refc, 0);
	w->bio = NULL;
	w->dma_ops = &nvme_dma_ops;
}

int nvme_open(void *opaque)
{
	return 0;
}

void nvme_release(void *opaque)
{
	return;
}

/**
 * nvme_init - Initialize NVMe
 *
 * @retval 0: Success
 *
 * DMA(NVMe) operation registering \n
 * Communicate data to NVMe using a Block I/O device
 */
int nvme_init(void)
{
	nvme_dma_ops.create_conn = create_conn;
	nvme_dma_ops.wait_for_conn = wait_for_conn;
	nvme_dma_ops.destroy_conn = destroy_conn;

	nvme_dma_ops.wait_read = emp_bdev_wait_read;
	nvme_dma_ops.try_wait_read = emp_bdev_try_wait_read;
	nvme_dma_ops.wait_write = emp_bdev_wait_write;
	nvme_dma_ops.post_read = emp_bdev_post_read;
	nvme_dma_ops.post_write = emp_bdev_post_write;

	nvme_dma_ops.setup_local_page = blk_create_local_page;
	nvme_dma_ops.destroy_local_page = blk_destroy_local_page;
	nvme_dma_ops.wr_ctor = nvme_wr_ctor;
	nvme_dma_ops.init_wr = nvme_init_wr;

	nvme_dma_ops.test_conn = test_bdev;

	return 0;
}

void nvme_exit(void)
{
	return;
}
#endif // CONFIG_EMP_BLOCKDEV
