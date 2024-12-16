#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <linux/uio.h>
#include <linux/sched/sysctl.h>
#include "vm.h"
#include "subblock.h"
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0))
#include <linux/genhd.h>
#endif

/**
 * dma_open - Create DMA work request cache & open RDMA or NVMe connection
 * @param opaque bvma data structure
 *
 * @retval 0: Success
 * @retval n: Error
 */
int dma_open(void *opaque)
{
	return 0;
}

/**
 * dma_release - Destroy DMA work request cache & release RDMA or NVMe connection
 * @param opaque bvma data structure
 */
void dma_release(void *opaque)
{
}

int dma_init(void)
{
	return 0;
}

void dma_exit(void)
{
	return;
}
