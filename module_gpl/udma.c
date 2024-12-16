#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <linux/uio.h>
#include <linux/sched/sysctl.h>
#ifdef CONFIG_EMP_BLOCKDEV
#include "nvme.h"
#endif
#ifdef CONFIG_EMP_RDMA
#include "rdma.h"
#endif
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
#ifdef CONFIG_EMP_BLOCKDEV
	nvme_open(opaque);
#endif
#ifdef CONFIG_EMP_RDMA
	rdma_open(opaque);
#endif
	return 0;
}

/**
 * dma_release - Destroy DMA work request cache & release RDMA or NVMe connection
 * @param opaque bvma data structure
 */
void dma_release(void *opaque)
{
#ifdef CONFIG_EMP_BLOCKDEV
	nvme_release(opaque);
#endif
#ifdef CONFIG_EMP_RDMA
	rdma_release(opaque);
#endif
}

int dma_init(void)
{
#ifdef CONFIG_EMP_BLOCKDEV
	nvme_init();
#endif
#ifdef CONFIG_EMP_RDMA
	rdma_init();
#endif
	return 0;
}

void dma_exit(void)
{
#ifdef CONFIG_EMP_BLOCKDEV
	nvme_exit();
#endif
#ifdef CONFIG_EMP_RDMA
	rdma_exit();
#endif
	return;
}
