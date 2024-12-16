#include "vm.h"
#include "glue.h"
#include "gpa.h"
#include "mm.h"
#include "reclaim.h"
#include "iov.h"
#include "donor_mem_rw.h"
#include "alloc.h"
#include "hva.h"
#include "page_mgmt.h"
#include "udma.h"
#include "remote_page.h"
#ifdef CONFIG_EMP_STAT
#include "stat.h"
#endif
#include "donor_mgmt.h"

#define MEMREG_SIZE (sizeof(struct memreg))

/**
 * create_mr - Create memory regions according to the type of donor
 * @param bvma bvma data structure
 * @param donor donor info
 * @param mr_id existed mr
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * Build connection to the donor and create memory regions
 */
static int create_mr(struct emp_mm *bvma, struct donor_info *donor, int *mr_id)
{
	int ret, mrid;
	struct memreg *mr;
	char str_buffer[16];
	struct connection *conn = NULL;
	struct dma_ops *dma_ops = NULL;
	unsigned long bitmap_size;

	/* NOTE: DONOR_DEV_DRAM and DONOR_DEV_PMEM are handled in library or qemu.
	 *       DONOR_DEV_DRAM: handled as ioctl
	 *       DONOR_DEV_PMEM: handled as DONOR_DEV_NVME
	 */
	switch (donor->dev_type) {
#ifdef CONFIG_EMP_BLOCKDEV
	case DONOR_DEV_NVME:
		dma_ops = &nvme_dma_ops;
		break;
#endif
	default:
		printk(KERN_ERR "donor type %d requested\n",  donor->dev_type);
		ret = -EINVAL;
		goto err;
	}

	ret = dma_ops->create_conn(bvma, &conn, donor);
	if (ret) {
		printk(KERN_ERR "failed to connect to the donor.\n");
		ret = -EINVAL;
		goto err;
	}

	mr = emp_kzalloc(MEMREG_SIZE, GFP_KERNEL);
	if (!mr) {
		ret = -ENOMEM;
		goto err_alloc_mr;
	}
	spin_lock_init(&mr->lock);

	mr->bvma = bvma;
	/* It assumes that size is in megabyte.
	 * Therefore, it is converted to pfn.  */
	mr->size = MB_TO_PAGE(donor->size);
	atomic64_set(&mr->alloc_len, 0);
	atomic_set(&mr->wr_len, 0);
	mr->dma_order = bvma_subblock_order(bvma);

	bitmap_size = sizeof(unsigned long) *
		      BITS_TO_LONGS(mr->size >> mr->dma_order);
	mr->alloc_bitmap = emp_vmalloc(bitmap_size);
	if (!mr->alloc_bitmap) {
		printk("failed to allocate bitmap for mr %s", donor->path);
		ret = -ENOMEM;
		goto err_alloc_bitmap_err;
	}
	bitmap_fill(mr->alloc_bitmap, mr->size >> mr->dma_order);
	mr->free_start = 0;
	mr->alloc_next = 0;
	
	mr->conn = conn;
	mr->ops = dma_ops;
	switch (donor->dev_type) {
#ifdef CONFIG_EMP_BLOCKDEV
	case DONOR_DEV_NVME:
		mr->type = MR_NVME;
		break;
#endif
	}

	conn->memreg = mr;
	conn->emm = bvma;

	mrid = alloc_mrid(bvma, mr);
	if (mrid < 0) {
		ret = -EBUSY;
		goto err_alloc_mrid;
	}
	mr->id = (u8)mrid;
	snprintf(str_buffer, 16, "memreg%d:%d", bvma->id, mr->id);
	mr->wr_cache = emp_kmem_cache_create(str_buffer,
			sizeof(struct work_request), 0, 0, dma_ops->wr_ctor);
	if (!mr->wr_cache) {
		ret = -ENOMEM;
		goto err_alloc_wr_cache;
	}
	init_waitqueue_head(&mr->wr_wq);

	// end of initialization
	if (mr_id)
		*mr_id = mr->id;

	adjust_remote_page_policy(bvma, mr);

	// disable CPF when donor is not RDMA
	if (!IS_MR_TYPE_RDMA(mr) && bvma->config.critical_page_first) {
		printk(KERN_ERR "Disable CPF when donor is not RDMA device\n");
		bvma->config.critical_page_first = 0;
	}

#ifdef CONFIG_EMP_BLOCKDEV
	if (mr->type == MR_NVME)
		bvma->mrs.blockdev_used = true;
#endif

	return 0;
err_alloc_wr_cache:
	free_mrid(bvma, mrid, false);
err_alloc_mrid:
	emp_vfree(mr->alloc_bitmap);
err_alloc_bitmap_err:
	emp_kfree(mr);
err_alloc_mr:
	(dma_ops->destroy_conn)(bvma, conn);
err:
	return ret;
}

/**
 * check_creation_mrs - Check the creations of memory region & connections for RDMA
 * @param bvma bvma data structure
 *
 * @retval 0: Success
 * @retval -1: Error
 *
 * Check the parallelized connections \n
 * TODO: This function is related in RDMA connection & memory regions.
 * Should be moved to rdma.c?
 */
int check_creation_mrs(struct emp_mm* bvma)
{
	struct connection *c;
	struct memreg *mr;
	struct dma_ops *ops;
	int i, ret = 0, failed = 0;
#ifdef CONFIG_EMP_OPT
	int l;
#endif

	for (i = 0; i < MAX_MRID; i++) {
		spin_lock(&bvma->mrs.memregs_lock);
		mr = bvma->mrs.memregs[i];
		spin_unlock(&bvma->mrs.memregs_lock);
		if (!mr)
			continue;

		c = mr->conn;
		ops = (struct dma_ops *)mr->ops;
		ret = ops->wait_for_conn(c);
		if (ret) {
			failed = true;
			continue;
		}

#ifdef CONFIG_EMP_OPT
		if (!bvma->config.eval_media || c->latency)
			continue;

		if ((l = ops->test_conn(c)) < 0)
			pr_info("conn%d test_conn failed ret: %d state: %d ",
					conn_get_mr_id(c), l,
					conn_get_ctrl_state(c));
		c->latency = l;
#endif
	}

	return failed;
}

/**
 * disconnect_mr - destroy the memory regiester and connection for a single donor
 * @param mr memory region to destroy
 *
 * Destroy connection to a donor and free memory region
 */
static void disconnect_mr(struct memreg *mr)
{
	if (mr->conn) {
		struct dma_ops *ops = mr->ops;

		(ops->destroy_conn)(mr->bvma, mr->conn);
	}
	if (mr->wr_cache) {
		// wait for that corresponding work requests are released
		mr->state = MR_STATE_CLOSING;
		wait_event_interruptible(mr->wr_wq,
				(atomic_read(&mr->wr_len) == 0));
		emp_kmem_cache_destroy(mr->wr_cache);
		mr->wr_cache = NULL;
	}
	if (mr->alloc_bitmap) {
		emp_vfree(mr->alloc_bitmap);
		mr->alloc_bitmap = NULL;
	}
	emp_kfree(mr);
}

/**
 * disconnect_mrs - destroy the all the memory regions and connections to donors
 * @param bvma bvma data structure
 *
 * Destroy connections to donors and free memory region
 */
static void disconnect_mrs(struct emp_mm *bvma)
{
	int i;
	struct memreg *mr;

	dprintk("try to destroy the connection: %d\n", bvma->mrs.memregs_len);
	for (i = 0; i < MAX_MRID; i++) {
		spin_lock(&bvma->mrs.memregs_lock);
		mr = bvma->mrs.memregs[i];
		if (mr)
			free_mrid(bvma, i, true);
		spin_unlock(&bvma->mrs.memregs_lock);
		if (mr)
			disconnect_mr(mr);
	}
	printk(KERN_NOTICE "complete to destroy the connection: %d\n",
		   bvma->mrs.memregs_len);

	debug_BUG_ON(bvma->mrs.memregs_len != 0);
}

/**
 * donor_mgmt_init - Initialize memory regions
 * @param bvma bvma data structure
 */
int donor_mgmt_init(struct emp_mm *bvma) 
{
	unsigned int memregs_size;

	memregs_size = sizeof(struct memreg *) * 
		(sizeof(bvma->mrs.memregs_len) << BITS_PER_BYTE);

	bvma->mrs.memregs = 
		(struct memreg **)emp_kzalloc(memregs_size, GFP_KERNEL);
	if (!bvma->mrs.memregs)
		return -ENOMEM;
	
	bvma->mops.create_mr = create_mr;
	bvma->mops.disconnect_mr = disconnect_mr;
	return 0;
}

/**
 * donor_mgmt_exit - Disconnect all memory regions
 * @param bvma bvma data structure
 */
void donor_mgmt_exit(struct emp_mm *bvma) {
	disconnect_mrs(bvma);
	emp_kfree(bvma->mrs.memregs);
}
