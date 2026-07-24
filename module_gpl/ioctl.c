#include <linux/module.h>
#include <linux/file.h>
#include "emp_ioctl.h"
#include "vm.h"
#include "iov.h"
#include "donor_mgmt.h"
#include "emp_madvise.h"
#include "reclaim.h"
#include "ioctl.h"

/**
 * realloc_donor_info - Allocate donor's memory info
 * @param d new donor memory info
 * @param usr_d donor memory info from user
 *
 * @retval NULL: Error
 * @retval n: Success
 *
 * Allocate a donor's data structure with donor's memory information
 */
static struct donor_info *realloc_donor_info(struct donor_info *d,
		struct donor_info *usr_d)
{
	struct donor_info *donor;
	unsigned int uret;

	if (d->path_len == 0)
		return d;

	donor = emp_kmalloc(sizeof(struct donor_info) + d->path_len, GFP_KERNEL);
	if (donor == NULL)
		return NULL;

	memcpy(donor, d, sizeof(struct donor_info));
	uret = copy_from_user(donor->path, (void __user *)usr_d->path,
			d->path_len);
	if (uret) {
		emp_kfree(donor);
		return NULL;
	}

	return donor;
}

static long __conn_donor(struct emp_mm *emm, unsigned long ioctl_param)
{
	unsigned int uret;
	long ret;
	struct donor_info _donor, *donor;

	uret = copy_from_user(&_donor, (void __user *)ioctl_param,
			sizeof(_donor));
	if (uret)
		return -EINVAL;

	donor = realloc_donor_info(&_donor, (struct donor_info *)ioctl_param);
	if (donor == NULL)
		return -EINVAL;

	ret = emm->mops.create_mr(emm, donor, NULL);
	if (donor != &_donor)
		emp_kfree(donor);
	return ret;
}

static long __fini_conn(struct emp_mm *emm)
{
	unsigned int uret;
	uret = check_creation_mrs(emm);
	printk(KERN_INFO "check creation mrs: %s",
			(uret == 0)? "success":"failed");
	return uret ? -EINVAL : 0;
}

static long __set_dram(struct emp_mm *emm, unsigned long ioctl_param)
{
	unsigned int uret;
	struct donor_info _donor, *donor;
	uret = copy_from_user(&_donor, (void __user *)ioctl_param,
			sizeof(_donor));
	if (uret)
		return -EINVAL;
	donor = &_donor;
	atomic_set(&emm->ftm.local_cache_pages, MB_TO_PAGE(donor->size));
#ifdef CONFIG_EMP_EXT
	emp_ops.reclaim_set(emm);
#else
	reclaim_set(emm);
#endif
	printk(KERN_INFO "set dram capacity: %ld MiB\n",
			donor->size);
	return 0;
}

#ifdef CONFIG_EMP_VM
int register_kvm(struct emp_mm *emm, int kvm_fd, int kvm_max_vcpus);
static long __reg_kvm(struct emp_mm *emm, unsigned long ioctl_param)
{
	size_t size;
	struct {
		int kvm_fd;
		int max_cpus;
	} reg_kvm;
	int kvm_fd, kvm_max_vcpus;
	size = copy_from_user(&reg_kvm, (void __user *)ioctl_param,
			sizeof(reg_kvm));
	if (size)
		return -EINVAL;
	kvm_fd = reg_kvm.kvm_fd;
	kvm_max_vcpus = reg_kvm.max_cpus;
	return register_kvm(emm, kvm_fd, kvm_max_vcpus);
}
#endif /* CONFIG_EMP_VM */

#ifdef CONFIG_EMP_IO
static long __hint_iov(struct emp_mm *emm, unsigned long ioctl_param,
							bool is_write)
{
	size_t size;
	struct QEMUIOVector qiov, *qiov_req;
	struct iovec *iov;
	size_t niov;
	// for now, turn off iov handling in case of multi-order page
	qiov_req = (struct QEMUIOVector *)ioctl_param;
	size = copy_from_user(&qiov, qiov_req, sizeof(struct QEMUIOVector));
	if (size) {
		printk("failed: copy_from_user: qiov %ld\n", size);
		return -EINVAL;
	}
	niov = qiov.niov;
	iov = emp_kmalloc(sizeof(struct iovec) * niov, GFP_KERNEL);
	if (!iov)
		return -ENOMEM;

	size = copy_from_user(iov, qiov.iov,
			sizeof(struct iovec) * niov);
	if (size) {
		printk("failed: copy_from_user: iov %ld\n", size);
		emp_kfree(iov);
		return -EINVAL;
	}
	handle_qiov(emm, is_write, iov, niov);
	emp_kfree(iov);
	return 0;
}
#endif /* CONFIG_EMP_IO */

#ifdef CONFIG_EMP_VM
void register_mem_slot(struct emp_mm *emm, unsigned long start, unsigned long size);
static long __reg_mem_region(struct emp_mm *emm, unsigned long ioctl_param)
{
	size_t size;
	struct {
		unsigned long start;
		unsigned long size;
	} memreg;
	size = copy_from_user(&memreg, (void __user *)ioctl_param, sizeof(memreg));
	if (size)
		return -EINVAL;
	register_mem_slot(emm, memreg.start, memreg.size);
	return 0;
}
#endif

static long __emp_madvise(struct emp_mm *emm, unsigned long ioctl_param)
{
	size_t size;
	struct emp_madv_info info;
	size = copy_from_user(&info,
			(struct emp_madv_info *) ioctl_param,
			sizeof(struct emp_madv_info));
	if (size) {
		printk("failed: copy_from_user: madv_info %ld\n", size);
		return -EINVAL;
	}

	switch (info.advice) {
	// *** codes from posix ***
	// case MADV_EMP_NORMAL: break;
	// case MADV_EMP_RANDOM: break;
	// case MADV_EMP_SEQUENTIAL: break;
	case MADV_EMP_WILLNEED:
			return emp_blk_prefetch(emm, info.addr, info.size);
	// case MADV_EMP_DONTNEED: break;

	// *** codes from linux ***
	// case MADV_EMP_REMOVE: break;
	// case MADV_EMP_DONTFORK: break;
	// case MADV_EMP_DOFORK: break;
	// case MADV_EMP_HWPOISON: break;
	// case MADV_EMP_MERGEABLE: break;
	// case MADV_EMP_UNMERGEABLE: break;
	// case MADV_EMP_SOFT_OFFLINE: break;
	// case MADV_EMP_HUGEPAGE: break;
	// case MADV_EMP_NOHUGEPAGE: break;
	// case MADV_EMP_DONTDUMP: break;
	// case MADV_EMP_DODUMP: break;
	// case MADV_EMP_FREE: break;
	// case MADV_EMP_WIPEONFORK: break;
	// case MADV_EMP_KEEPONFORK: break;
	// case MADV_EMP_COLD: break;
	// case MADV_EMP_PAGEOUT: break;

	// *** EMP only codes ***
	// case MADV_EMP_PIN: break;
	// case MADV_EMP_UNPIN: break;

	default:
			return -EINVAL;
	}
}

/**
 * emp_unlocked_ioctl - Provide a communication channel between QEMU and EMP module
 * @param file device file
 * @param ioctl_num IOCTL number
 * @param ioctl_param IOCTL parameters
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * Communicate QEMU with the information listed below
 * + IOCTL_GET_LOWER_SIZE
 * + IOCTL_CONN_DONOR
 * + IOCTL_SET_DRAM
 * + IOCTL_REG_KVM
 * + IOCTL_HINT_IOV_W/R
 * + IOCTL_REG_MEM_REGION
 * + IOCTL_CONNECT_EM
 */
long emp_unlocked_ioctl(struct file *file, unsigned int ioctl_num,
		unsigned long ioctl_param)
{
	int ret = 0;
	struct emp_mm *emm;

	if (!file->private_data)
		return -ENODEV;

	emm = (struct emp_mm *)file->private_data;
#ifdef CONFIG_EMP_EXT
	if (emp_ext.emp_unlocked_ioctl
			&& emp_ext.emp_unlocked_ioctl(emm, ioctl_num, ioctl_param) == 0)
		return 0;
#endif
	switch (ioctl_num) {
		case IOCTL_GET_LOWER_SIZE:
			ret = LOWER_MEM_SIZE;
			break;

		case IOCTL_CONN_DONOR:
			ret = __conn_donor(emm, ioctl_param);
			break;

		case IOCTL_FINI_CONN:
			ret = __fini_conn(emm);
			break;

		case IOCTL_SET_DRAM:
			ret = __set_dram(emm, ioctl_param);
			break;

#ifdef CONFIG_EMP_VM
		case IOCTL_REG_KVM:
			ret = __reg_kvm(emm, ioctl_param);
			break;
#endif /* CONFIG_EMP_VM */

#ifdef CONFIG_EMP_IO
		case IOCTL_HINT_IOV_W:
			ret = __hint_iov(emm, ioctl_param, true);
			break;
		case IOCTL_HINT_IOV_R:
			ret = __hint_iov(emm, ioctl_param, false);
			break;
#endif
#ifdef CONFIG_EMP_VM
		case IOCTL_REG_MEM_REGION:
			ret = __reg_mem_region(emm, ioctl_param);
			break;
#endif
		case IOCTL_EMP_MADV:
			ret = __emp_madvise(emm, ioctl_param);
			break;

		default:
			printk(KERN_ERR "unknown ioctl called %d\n", ioctl_num);
			ret = -EINVAL;
			break;
	}

	return ret;
}
