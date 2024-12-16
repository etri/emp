#include <linux/module.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/swap.h>
#include <linux/mmu_notifier.h>
#include <linux/list.h>
#include <linux/semaphore.h>
#include <asm/bitops.h>
#include "config.h"
#include "gpa.h"
#include "emp_ioctl.h"
#include "vm.h"
#include "glue.h"
#include "mm.h"
#include "block.h"
#include "reclaim.h"
#include "iov.h"
#include "donor_mem_rw.h"
#include "alloc.h"
#include "hva.h"
#include "page_mgmt.h"
#include "udma.h"
#include "donor_mgmt.h"
#include "debug.h"
#include "kvm_emp.h"
#include "procfs.h"
#include "local_page.h"
#include "block-flag.h"
#include "debug.h"

#undef MEASURE_COMPONENTS

/* ----- global variables ----- */
size_t  initial_local_cache_pages; // counts in 4kb
int     initial_remote_reuse;
int     initial_remote_policy_subblock;
int     initial_remote_policy_block;

#ifdef CONFIG_EMP_SHOW_FAULT_PROGRESS
#ifdef CONFIG_EMP_VM
atomic64_t num_emp_gpa_fault = {0};
#endif
atomic64_t num_emp_hva_fault = {0};
#endif

/* ----- local variables ----- */
static int                  emp_major = 0;
static struct class         *emp_class;
struct emp_mm               **emp_mm_arr;
unsigned long               emp_mm_arr_len;
spinlock_t                  emp_mm_arr_lock;
/* ----- local variables ----- */

#define BVMA_SIZE (sizeof(struct emp_mm))

/* ----- functions from procfs.c ----- */
int emp_procfs_add(struct emp_mm *bvma, int id);
void emp_procfs_del(struct emp_mm *bvma);
int emp_procfs_init(void);
void emp_procfs_exit(void);
	

/**
 * get_emp_mm_arr - Get the whole bvma array
 *
 * @return array of bvma
 */
struct emp_mm **get_emp_mm_arr(void)
{
	return emp_mm_arr;
}

#ifdef CONFIG_EMP_VM
static inline void set_kvm_emp_mm(struct kvm *kvm, void *emp_mm) {
	container_of(kvm, struct kvm_emp_container, kvm)->emp_mm = emp_mm;	
}
#endif

static void init_vcpu_var(struct vcpu_var *v, int id) 
{
	memset(v, 0, sizeof(*v));
	init_emp_list(&v->local_free_page_list);
	spin_lock_init(&(v)->wb_request_lock);
	INIT_LIST_HEAD(&(v)->wb_request_list);
	(v)->post_writeback.length = 1;
	(v)->post_writeback.weight = 2;
	(v)->id = id;
}

#ifdef CONFIG_EMP_VM
static void put_vcpus_var(struct emp_mm *emm);
/**
 * get_vcpus_var - Initialize vcpu_var data structure
 * @param bvma bvma data structure
 */
static int get_vcpus_var(struct emp_mm *bvma)
{
	struct vcpu_var *v;
	int i, vcpus_size, vcpus_len = EMP_KVM_VCPU_LEN(bvma);

	vcpus_size = sizeof(struct vcpu_var) * vcpus_len;
	bvma->vcpus = emp_kmalloc(vcpus_size, GFP_KERNEL);
	if (!bvma->vcpus)
		goto get_vcpus_var_fail;

	for (i = 0; i < vcpus_len; i++) {
		v = &bvma->vcpus[i];
		init_vcpu_var(v, i);
	}
	return 0;

get_vcpus_var_fail:
	if (bvma->vcpus)
		put_vcpus_var(bvma);
	return -ENOMEM;
}

static void __put_vcpus_var(struct emp_mm *bvma, int cpus_len)
{
	int cpu;
	struct vcpu_var *v;
	struct emp_list *free_page_list;

	if (bvma->vcpus == NULL)
		return;

	free_page_list = &bvma->ftm.free_page_list;
	emp_list_lock(free_page_list);
	for (cpu = 0; cpu < cpus_len; cpu++) {
		v = &bvma->vcpus[cpu];
		flush_local_free_pages(bvma, v);
	}
	emp_list_unlock(free_page_list);

	emp_kfree(bvma->vcpus);
	bvma->vcpus = NULL;
}

/**
 * put_vcpus_var - Free vcpu_var data structure
 * @param emm emm data structure
 */
static void put_vcpus_var(struct emp_mm *emm)
{
	__put_vcpus_var(emm, EMP_KVM_VCPU_LEN(emm));
}
#endif /* CONFIG_EMP_VM */

static int get_pcpus_var(struct emp_mm *emm)
{
	int cpu;
	struct vcpu_var *v;

	emm->pcpus = emp_alloc_percpu(struct vcpu_var);
	if (emm->pcpus == NULL)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		v = per_cpu_ptr(emm->pcpus, cpu);
		init_vcpu_var(v, VCPU_ID(emm, cpu));
	}
	return 0;
}

static void put_pcpus_var(struct emp_mm *emm)
{
	int cpu;
	struct vcpu_var *v;
	struct emp_list *free_page_list;

	if (emm->pcpus == NULL)
		return;

	free_page_list = &emm->ftm.free_page_list;
	emp_list_lock(free_page_list);
	for_each_possible_cpu(cpu) {
		v = per_cpu_ptr(emm->pcpus, cpu);
		flush_local_free_pages(emm, v);
	}
	emp_list_unlock(free_page_list);

	emp_free_percpu(emm->pcpus);
	emm->pcpus = NULL;
}

static int emp_vmr_find_and_set(struct emp_mm *emm, struct emp_vmr *vmr)
{
	unsigned long p;
	p = find_first_bit(emm->vmrs_bitmap, EMP_VMRS_MAX);
	if (unlikely(p == EMP_VMRS_MAX))
		return -1;

	vmr->id = p;
	emm->vmrs[p] = vmr;
	__clear_bit(p, emm->vmrs_bitmap);
	emm->vmrs_len++;

	return p;
}

static void emp_vmr_release(struct emp_vmr *vmr)
{
	struct emp_mm *emm = vmr->emm;

	vmr->magic = 0; // remove the magic value

	if (emm->last_vmr == vmr)
		emm->last_vmr = NULL;

	emm->vmrs[vmr->id] = NULL;
	__set_bit(vmr->id, emm->vmrs_bitmap);
	emm->vmrs_len--;
}

static void emp_vma_close(struct vm_area_struct *vma)
{
	struct emp_vmr *vmr;

	vmr = __get_emp_vmr(vma);
	if (vmr == NULL)
		return;

	printk(KERN_NOTICE "%s emm_id: %d vmr_id: %d vma:%p virt %lx vmr: %lx\n",
				__func__, vmr->emm->id, vmr->id,
				vma, vma->vm_start, (unsigned long) vmr);

	/* vmr->vmr_closing may be set by mmu notifier */
	if (vmr->vmr_closing == false) {
		vmr->vmr_closing = true;
		smp_mb();
		debug_show_gpa_state(vmr, __func__);
	}



	/* gpas_close() may have been called by mmu notifier.
	 * In such case, vmr->descs == NULL and nothing happens by gpas_close().
	 */
	gpas_close(vmr, false); // also free the remote page

#ifdef CONFIG_EMP_DEBUG_RSS
	emp_update_rss_show(vmr);
#endif

	emp_vmr_release(vmr);


	vmr->emm->last_mm = vma->vm_mm;
	vmr->host_vma = NULL;
	vmr->host_mm = NULL;
	emp_kfree(vmr);
}


static struct emp_vmr *create_vmr(struct emp_mm *emm)
{
	const size_t emp_vmr_size = sizeof(struct emp_vmr);
	struct emp_vmr *new_vmr;

	new_vmr = emp_kzalloc(emp_vmr_size, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(new_vmr))
		return NULL;

	if (emp_vmr_find_and_set(emm, new_vmr) < 0) {
		emp_kfree(new_vmr);
		return NULL;
	}

	new_vmr->magic = EMP_VMR_MAGIC_VALUE;
	new_vmr->emm = emm;
	new_vmr->pvid = -1;
	new_vmr->new_gpadesc = new_gpadesc;
#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC
	new_vmr->set_gpadesc_alloc_at = set_gpadesc_alloc_at;
#endif

	spin_lock_init(&new_vmr->gpas_close_lock);


	return new_vmr;
}


static struct vm_operations_struct emp_vma_ops = {
	.close = emp_vma_close,
	.fault = emp_page_fault_hva,
};

/**
 * emp_mmap - mmap for EMP
 * @param filp emp device file pointer
 * @param vma virtual memory area info of EMP
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * This function is called in QEMU's function when mapping VM's memory
 * on the host memory. \n
 * It maps memory and initializes EMP's components
 */
static int COMPILER_DEBUG
emp_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret = 0, i;
	struct emp_mm *bvma;
	size_t mem_size;
	struct emp_vmr *vmr;
#ifdef CONFIG_EMP_VM
	struct mm_struct *mm = vma->vm_mm;
	unsigned long new_start = vma->vm_start;
	unsigned long new_end = vma->vm_end;
#endif

	if (!filp->private_data)
		return -ENODEV;
	bvma = (struct emp_mm *)filp->private_data;

	dprintk("%s (1) mm:%016lx vma:%016lx vm_flags: 0x%016lx "
		"vm_start: 0x%lx vm_end: 0x%lx\n", __func__,
		(unsigned long) vma->vm_mm, (unsigned long) vma,
		vma->vm_flags, vma->vm_start, vma->vm_end);

#ifdef CONFIG_EMP_VM
	if (bvma->ekvm.kvm &&
			((vma->vm_start & ~HPAGE_MASK) || 
			 (vma->vm_end & ~HPAGE_MASK))) {
		unsigned long old_start, old_end;
		size_t length;
		off_t shift;
		struct vm_area_struct *new_vma;

		old_start = vma->vm_start;
		old_end = vma->vm_end;
		length = old_end - old_start;

		shift = vma->vm_start & ~HPAGE_MASK;
		new_start = vma->vm_start - shift;
		new_end = vma->vm_end - shift;

		new_vma = find_vma(mm, new_start);
		if (new_end <= new_vma->vm_start)
			goto vm_start_aligned;

		shift = HPAGE_SIZE - shift;
		new_start = vma->vm_start + shift;
		new_end = vma->vm_end + shift;

		new_vma = find_vma(mm, new_start);
		if (new_end <= new_vma->vm_start)
			goto vm_start_aligned;

		return -EFAULT;
	}

vm_start_aligned:
	if (vma->vm_start != new_start) {
		vma->vm_start = new_start;
		vma->vm_end = new_end;
		dprintk("%s aligned vm_start: 0x%lx vm_end: 0x%lx\n",
				__func__, new_start, new_end);
	}
#endif /* CONFIG_EMP_VM */


	might_sleep();

	// check whether requested memory size can be served by donor
	mem_size = 0;
	if (!bvma->config.remote_reuse)
		mem_size += atomic_read(&bvma->ftm.local_cache_pages);
	spin_lock(&bvma->mrs.memregs_lock);
	for (i = 0; i < MAX_MRID; i++) {
		if (bvma->mrs.memregs[i] == NULL)
			continue;
		mem_size += bvma->mrs.memregs[i]->size;
	}
	spin_unlock(&bvma->mrs.memregs_lock);
	mem_size <<= PAGE_SHIFT;
	if (mem_size < (vma->vm_end - vma->vm_start))
		return -ENOMEM;

	vmr = create_vmr(bvma);
	if (vmr == NULL)
		return -ENOMEM;
	vmr->host_vma = vma;
	vmr->host_mm = vma->vm_mm;


	if (gpas_open(vmr))
		goto mmap_fail;

	bvma->ftm.local_cache_pages_headroom = LOCAL_CACHE_BUFFER_SIZE(bvma);

	// prevent numa from relocating the related pages
	vma->vm_flags |= VM_MIXEDMAP;
	vma->vm_flags |= VM_NOHUGEPAGE;
	vma->vm_flags |= VM_DONTEXPAND;

	vma->vm_ops = &emp_vma_ops;
	vma->vm_private_data = (void *)vmr;

#ifdef CONFIG_EMP_VM
	// GPN_TO_HVA requires vmr.host_vma
	if (bvma->ekvm.kvm && (bvma->ekvm.apic_base_hva == 0UL))
		bvma->ekvm.apic_base_hva = GPN_TO_HVA(bvma, vmr,
				APIC_DEFAULT_PHYS_BASE >> PAGE_SHIFT);
#endif

	dprintk("%s (2) mm:%016lx vma:%016lx vm_flags: 0x%016lx "
		"vm_start: 0x%lx vm_end: 0x%lx\n", __func__,
		(unsigned long) vma->vm_mm, (unsigned long) vma,
		vma->vm_flags, vma->vm_start, vma->vm_end);
	return ret;

mmap_fail:
	gpas_close(vmr, false);
	emp_vmr_release(vmr);
	return -ENOMEM;
}

#ifdef CONFIG_EMP_VM
/**
 * register_mem_slot - register memory slot using vma info
 * @param bvma bvma data structure
 * @param start start address of vm's memory
 * @param size vm's total memory size
 *
 * Register memslot using vm's memory info
 */
static void register_mem_slot(struct emp_mm *bvma, unsigned long start, unsigned long size)
{
	struct kvm_memory_slot *slot;

	debug_BUG_ON(bvma->ekvm.memslot_len > 2);

	slot = gfn_to_memslot(bvma->ekvm.kvm, gpa_to_gfn(start));
	if (slot) {
		int i, memslot;
		unsigned long base = 0;
		FOR_EACH_MEMSLOT(bvma, i)
			base += bvma->ekvm.memslot[i].size;
		memslot = bvma->ekvm.memslot_len++;
		bvma->ekvm.memslot[memslot].base = base;
		bvma->ekvm.memslot[memslot].gpa = start;
		bvma->ekvm.memslot[memslot].size = size;
		bvma->ekvm.memslot[memslot].hva =
			__gfn_to_hva_memslot(slot, gpa_to_gfn(start));

		dprintk("mem region registered: "
			"gpa: %lx hva: %lx size: %lx base:%lx\n",
			start, bvma->ekvm.memslot[memslot].hva, size, base);
	}
}
#endif /* CONFIG_EMP_VM */

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

#ifdef CONFIG_EMP_VM
/**
 * register_kvm - Register KVM functions for mapping EMP functions to use
 * @param bvma bvma data structure
 * @param kvm_fd kvm file descriptor
 * @param kvm_max_vcpus maximum vcpus
 *
 * @retval true: Success
 * @retval false: Error
 *
 * Replace functionalities of KVM to EMP's functions
 */
static int COMPILER_DEBUG
register_kvm(struct emp_mm *bvma, int kvm_fd, int kvm_max_vcpus)
{
	struct file *kvm_filp;

	kvm_filp = fget(kvm_fd);
	if (!kvm_filp)
		return -ENOENT;

#ifndef CONFIG_EMP_DONT_RESTRICT_LOW_MEMORY_REGION
	if (bvma->config.subblock_order > LOW_MEMORY_MAX_ORDER) {
		printk(KERN_ERR "ERROR: [emp] the maximum subblock order for VM"
				" should be less than or equal to"
				" LOW_MEMORY_MAX_ORDER(%d), but the subblock"
				" order is %d\n",
			LOW_MEMORY_MAX_ORDER,
			bvma->config.subblock_order);
		return -EINVAL;
	}
#endif

	bvma->ekvm.kvm = (struct kvm *)kvm_filp->private_data;
	set_kvm_emp_mm(bvma->ekvm.kvm, (void *)bvma);

	if (bvma->ekvm.kvm_vcpus_len == 0) {
		int prev_vcpus_len = EMP_KVM_VCPU_LEN(bvma);

		bvma->ekvm.kvm_vcpus_len = 
			atomic_read(&bvma->ekvm.kvm->online_vcpus);
		if (bvma->ekvm.kvm_vcpus_len == 0)
			bvma->ekvm.kvm_vcpus_len = kvm_max_vcpus;
		bvma->ekvm.emp_vcpus_len = bvma->ekvm.kvm_vcpus_len
						+ VCPU_START_ID;

		if (prev_vcpus_len < EMP_KVM_VCPU_LEN(bvma)) {
			__put_vcpus_var(bvma, prev_vcpus_len);
			reclaim_exit(bvma);

			if (get_vcpus_var(bvma))
				goto reg_kvm_get_vcpus_err;
			if (reclaim_init(bvma))
				goto reg_kvm_reclaim_init_err;
		}
	}

	printk(KERN_INFO "kvm is registered. the number of vcpus: %d\n",
			bvma->ekvm.kvm_vcpus_len);

	fput(kvm_filp);
	kvm_get_kvm(bvma->ekvm.kvm);

	return true;

reg_kvm_reclaim_init_err:
	put_vcpus_var(bvma);
reg_kvm_get_vcpus_err:
	return false;
}
#endif /* CONFIG_EMP_VM */

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
static long emp_unlocked_ioctl(struct file *file, unsigned int ioctl_num,
		unsigned long ioctl_param)
{
	int ret = 0;
	struct emp_mm *bvma;

	if (!file->private_data)
		return -ENODEV;

	bvma = (struct emp_mm *)file->private_data;
	switch (ioctl_num) {
		unsigned int uret;
		struct donor_info _donor, *donor;
		/*struct file *kvm_filp;*/
		size_t size;
#ifdef CONFIG_EMP_IO
		struct QEMUIOVector qiov, *qiov_req;
		struct iovec *iov;
		size_t niov;
#endif
		struct {
			unsigned long start;
			unsigned long size;
		} memreg;
#ifdef CONFIG_EMP_VM
		struct {
			int kvm_fd;
			int max_cpus;
		} reg_kvm;
		int kvm_fd, kvm_max_vcpus;
#endif /* CONFIG_EMP_VM */

		case IOCTL_GET_LOWER_SIZE:
			ret = LOWER_MEM_SIZE;
			break;

		case IOCTL_CONN_DONOR:
			uret = copy_from_user(&_donor,
					(void __user *)ioctl_param,
					sizeof(_donor));
			if (uret) {
				ret = -EINVAL;
				break;
			}
			
			donor = realloc_donor_info(&_donor, 
					(struct donor_info *)ioctl_param);
			if (donor == NULL) {
				ret = -EINVAL;
				break;
			}
			ret = bvma->mops.create_mr(bvma, donor, NULL);
			if (donor != &_donor)
				emp_kfree(donor);

			break;

		case IOCTL_FINI_CONN:
			uret = check_creation_mrs(bvma);
			printk(KERN_INFO "check creation mrs: %s",
					(uret == 0)? "success":"failed");
			if (uret)
				ret = -EINVAL;
			break;

		case IOCTL_SET_DRAM:
			uret = copy_from_user(&_donor,
					(void __user *)ioctl_param,
					sizeof(_donor));
			if (uret) {
				ret = -EINVAL;
				break;
			}
			donor = &_donor;
			atomic_set(&bvma->ftm.local_cache_pages,
					MB_TO_PAGE(donor->size));
			reclaim_set(bvma);
			printk(KERN_INFO "set dram capacity: %ld MiB\n",
					donor->size);
			break;

#ifdef CONFIG_EMP_VM
		case IOCTL_REG_KVM:
			size = copy_from_user(&reg_kvm, (void __user *)ioctl_param, 
					sizeof(reg_kvm));
			if (size) {
				ret = -EINVAL;
				break;
			}
			kvm_fd = reg_kvm.kvm_fd;
			kvm_max_vcpus = reg_kvm.max_cpus;
			ret = register_kvm(bvma, kvm_fd, kvm_max_vcpus);
			break;
#endif /* CONFIG_EMP_VM */

		case IOCTL_HINT_IOV_W:
		case IOCTL_HINT_IOV_R:
#ifdef CONFIG_EMP_IO
			// for now, turn off iov handling in case of multi-order page
			qiov_req = (struct QEMUIOVector *)ioctl_param;
			size = copy_from_user(&qiov, qiov_req, sizeof(struct QEMUIOVector));
			if (size) {
				printk("failed: copy_from_user: qiov %ld\n", size);
				ret = -EINVAL;
				break;
			}
			niov = qiov.niov;
			iov = emp_kmalloc(sizeof(struct iovec) * niov, GFP_KERNEL);
			if (!iov)
				break;

			size = copy_from_user(iov, qiov.iov,
					sizeof(struct iovec) * niov);
			if (size) {
				printk("failed: copy_from_user: iov %ld\n", size);
				ret = -EINVAL;
				emp_kfree(iov);
				break;
			}
			handle_qiov(bvma, ioctl_num==IOCTL_HINT_IOV_W, iov, niov);
			emp_kfree(iov);
#endif
			break;
		case IOCTL_REG_MEM_REGION:
			size = copy_from_user(&memreg, (void __user *)ioctl_param, sizeof(memreg));
			if (size) {
				ret = -EINVAL;
				break;
			}
#ifdef CONFIG_EMP_VM
			register_mem_slot(bvma, memreg.start, memreg.size);
#endif
			break;

		default:
			printk(KERN_ERR "unknown ioctl called %d\n", ioctl_num);
			ret = -EINVAL;
			break;
	}

	return ret;
}

/**
 * register_bvma - Initialize the virtual memory information for each VM used in EMP
 * @param bvma bvma data structure
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int register_bvma(struct emp_mm *bvma)
{
	int i, emp_index, ret;

	ret = -EBUSY;
	emp_index = -1;
	spin_lock(&emp_mm_arr_lock);
	for (i = 0; i < EMP_MM_MAX; i++) {
		if (emp_mm_arr[i] == NULL) {
			emp_index = i;
			break;
		}
	}
	if (emp_index != -1) {
		bvma->id = emp_index;
		emp_mm_arr_len++;
		emp_mm_arr[emp_index] = bvma;
		ret = 0;
	}
	spin_unlock(&emp_mm_arr_lock);

	return ret;
}

/**
 * unregister_bvma - Destroy the virtual memory information for each VM used in EMP
 * @param bvma bvma data structure
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int unregister_bvma(struct emp_mm *bvma)
{
	if (emp_mm_arr[bvma->id] == NULL ||
			emp_mm_arr[bvma->id] != bvma)
		return -EINVAL;

	spin_lock(&emp_mm_arr_lock);
	emp_mm_arr[bvma->id] = NULL;
	emp_mm_arr_len--;

	debug_unregister_bvma(bvma);
	spin_unlock(&emp_mm_arr_lock);
	return 0;
}

static DEFINE_MUTEX(emp_open_mutex);

static void cleanup_emm(struct emp_mm *emm)
{
	emm->close = 1;

	synchronize_rcu();
	synchronize_srcu(&emm->srcu);
	cleanup_srcu_struct(&emm->srcu);
}

static struct emp_mm *create_emm(void) 
{
	struct emp_mm *bvma;
	
	bvma = (struct emp_mm *)emp_kzalloc(BVMA_SIZE, GFP_KERNEL);
	if (!bvma) {
		printk(KERN_ERR "ERROR: failed to allocate bvma structure (size: %ld)\n",
							BVMA_SIZE);
		return NULL;
	}
	bvma->possible_cpus = num_possible_cpus();

	if (donor_mgmt_init(bvma))
		goto err;

	bvma->vmrs = emp_kzalloc(sizeof(struct emp_vmr*) * EMP_VMRS_MAX,
				 GFP_KERNEL);
	if (bvma->vmrs == NULL) {
		printk(KERN_ERR "ERROR: failed to allocate vmr pointers (size: %ld)\n",
							sizeof(struct emp_vmr*) * EMP_VMRS_MAX);
		goto err;
	}
	bitmap_fill(bvma->vmrs_bitmap, EMP_VMRS_MAX);

	init_srcu_struct(&bvma->srcu);
	spin_lock_init(&bvma->mrs.memregs_lock);
	init_waitqueue_head(&bvma->mrs.mrs_ctrl_wq);

	atomic_set(&bvma->refcount, 1);
	atomic_set(&bvma->ftm.alloc_pages_len, 0);

	init_emp_list(&bvma->ftm.free_page_list);
	init_waitqueue_head(&bvma->ftm.free_pages_wq);

	bvma->config.remote_reuse = initial_remote_reuse;
	bvma->config.remote_policy_subblock = initial_remote_policy_subblock;
	bvma->config.remote_policy_block = initial_remote_policy_block;
	atomic_set(&bvma->ftm.local_cache_pages, initial_local_cache_pages);

	bvma->ftm.local_cache_pages_headroom = 0;

	return bvma;
err:
	if (bvma->vmrs)
		emp_kfree(bvma->vmrs);
	emp_kfree(bvma);
	return NULL;
}

static void free_bvma(struct emp_mm *bvma) 
{
	if (!bvma) return;
	if (bvma->mrs.memregs)
		emp_kfree(bvma->mrs.memregs);
	emp_kfree(bvma);
}

/**
 * @param filp EMP device file pointer
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int emp_open(struct inode *inode, struct file *filp)
{
	int ret = 0;
	struct emp_mm *bvma;

	dprintk("%s[%d] f_flags: 0x%x\n", __func__, __LINE__, filp->f_flags);

	try_module_get(THIS_MODULE);
	mutex_lock(&emp_open_mutex);

	if ((filp->f_flags & O_ACCMODE) == O_RDONLY) {
		printk(KERN_ERR "%s: failed to open: inappropriate flags\n",
				EMP_DEVICE_NAME);
		ret = -EINVAL;
		goto open_err;
	}

	if (emp_mm_arr_len >= EMP_MM_MAX) {
		printk(KERN_ERR "%s: failed to open: no vma slot\n",
				EMP_DEVICE_NAME);
		ret = -EBUSY;
		goto open_err;
	}

	bvma = create_emm();
	if (!bvma) {
		printk(KERN_ERR "%s: failed to open: lack of memory\n",
				EMP_DEVICE_NAME);
		ret = -ENOMEM;
		goto open_new_bvma_err;
	}

	donor_mem_rw_init(bvma);
	dma_open(bvma);
	filp->private_data = bvma;

	ret = register_bvma(bvma);
	if (ret != 0) {
		printk(KERN_ERR "%s: failed to register emp vma\n",
				EMP_DEVICE_NAME);
		goto open_register_err;
	}

	if (emp_procfs_add(bvma, bvma->id)) {
		ret = -ENOENT;
		goto open_procfs_err;
	}

	if (remote_page_init(bvma))
		goto open_rp_init_err;
#ifdef CONFIG_EMP_VM
	if (get_vcpus_var(bvma))
		goto open_alloc_vcpu_err;
#endif
	if (get_pcpus_var(bvma))
		goto open_get_pcpus_var;
	if (reclaim_init(bvma))
		goto open_reclaim_init_err;
	if (local_page_init(bvma))
		goto open_lp_init_err;
	gpa_init(bvma);

	bvma->pid = current->pid;

	mutex_unlock(&emp_open_mutex);
	dprintk("new bvma id:%d\n", bvma->id);
	dprintk("emp_open exit\n");
	return ret;

open_procfs_err:
	local_page_exit(bvma);
open_lp_init_err:
	reclaim_exit(bvma);
open_reclaim_init_err:
	put_pcpus_var(bvma);
open_get_pcpus_var:
#ifdef CONFIG_EMP_VM
	put_vcpus_var(bvma);
open_alloc_vcpu_err:
#endif /* CONFIG_EMP_VM */
	remote_page_exit(bvma);
open_rp_init_err:
	unregister_bvma(bvma);
	dma_release(bvma);
open_register_err:
	free_bvma(bvma);
open_new_bvma_err:
	filp->private_data = NULL;
open_err:
	mutex_unlock(&emp_open_mutex);
	module_put(THIS_MODULE);
	return ret;
}

#ifdef CONFIG_KVM_ALLOC_PROFILE
void kvm_alloc_show_stat(void);
#endif

/**
 * emp_release - Release bvma for VM's address space & unregister EMP data structures
 * @param inode inode data structure
 * @param filp EMP device file pointer
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int emp_release(struct inode *inode, struct file *filp)
{
	struct emp_mm *bvma;

	dprintk("%s: filp->private_data=0x%lx\n", __func__,
			(unsigned long)filp->private_data);

	if (!filp->private_data)
		return -ENODEV;
	bvma = (struct emp_mm *)filp->private_data;

	WARN_ON(atomic_dec_and_test(&bvma->refcount) != true);

	mutex_lock(&emp_open_mutex);

	cleanup_emm(bvma);
	reclaim_exit(bvma);
	gpa_exit(bvma);

#ifdef CONFIG_EMP_VM
	if (bvma->vcpus)
		put_vcpus_var(bvma);
#endif
	if (bvma->pcpus)
		put_pcpus_var(bvma);

	remote_page_exit(bvma);
	local_page_exit(bvma);
	alloc_exit(bvma);

	unregister_bvma(bvma);

	donor_mgmt_exit(bvma);
	dma_release(bvma);

#ifdef CONFIG_EMP_VM
	if (bvma->ekvm.kvm) {
		set_kvm_emp_mm(bvma->ekvm.kvm, NULL);

		kvm_put_kvm(bvma->ekvm.kvm);
	}
#endif

	emp_procfs_del(bvma);
	emp_kfree(bvma->vmrs);
	emp_kfree(bvma);

#ifdef CONFIG_KVM_ALLOC_PROFILE
	kvm_alloc_show_stat();
#endif

	mutex_unlock(&emp_open_mutex);
	module_put(THIS_MODULE);

	printk(KERN_INFO "emp_release exit.\n");

	return 0;
}

int emp_fsync(struct file *filp, loff_t s, loff_t e, int datasync)
{
	struct emp_mm *bvma;

	printk(KERN_DEBUG "%s called s: %llx e: %llx datasync: %d",
			__func__, s, e, datasync);

	if (!filp->private_data)
		return -EINVAL;

	bvma = (struct emp_mm *)filp->private_data;
	if (!bvma)
		return -EINVAL;

	return 0;
}

static const struct file_operations emp_fops = {
	.open		= emp_open,
	.release	= emp_release,
	.mmap		= emp_mmap,
	.unlocked_ioctl	= emp_unlocked_ioctl,
	.fsync		= emp_fsync,
};

static int emp_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}

static int check_assumption(void)
{
	if (sizeof(struct emp_gpa) != 64 && sizeof(struct emp_gpa) != 32) {
#ifdef CONFIG_EMP_DEBUG
		printk(KERN_WARNING "warning on data structure: emp_gpa: %ld != 64 or 32\n",
				sizeof(struct emp_gpa));
#else
		printk(KERN_ERR "error on data structure: emp_gpa: %ld != 64 or 32\n",
				sizeof(struct emp_gpa));
		return -EINVAL;
#endif
	} else
		dprintk(KERN_ERR "[EMP] emp_gpa descriptor size: %ld\n",
				sizeof(struct emp_gpa));
	return 0;
}

/**
 * emp_init - Initialize EMP module
 *
 * Initialize configurations and register character device
 */
static int __init emp_init(void)
{
	int ret = 0;
	struct device *err_dev;
#ifdef CONFIG_EMP_VM
	struct emp_mod emp_mod = {
		.page_fault = emp_page_fault_gpa,
		.mark_free_pages = NULL,
		.lock_range_pmd = emp_lock_range_pmd,
		.unlock_range_pmd = emp_unlock_range_pmd,
	};
#endif /* CONFIG_EMP_VM */

	if (check_assumption())
		return -EINVAL;

	emp_debug_alloc_init();

	kernel_symbol_init();
#ifdef CONFIG_EMP_VM
	register_emp_mod(&emp_mod);
#endif /* CONFIG_EMP_VM */

	emp_mm_arr = emp_kzalloc(EMP_MM_MAX * sizeof(struct emp_mm *), GFP_KERNEL);
	if (!emp_mm_arr) {
		printk(KERN_ERR "failed to allocate memory for vmas\n");
		return -ENOMEM;
	}
	emp_mm_arr_len = 0;
	spin_lock_init(&emp_mm_arr_lock);

	dma_init();

	//nvme_test_while_init(); /* for debug */
	//
	initial_local_cache_pages = LOCAL_CACHE_PAGES;
	initial_remote_reuse = DEFAULT_REMOTE_REUSE;
	initial_remote_policy_subblock = DEFAULT_REMOTE_POLICY_SUBBLOCK;
	initial_remote_policy_block = DEFAULT_REMOTE_POLICY_BLOCK;

	ret = emp_procfs_init();
	if (ret) {
		printk(KERN_ERR "unable to create procfs for emp\n");
		goto err;
	}

	emp_major = register_chrdev(0, EMP_DEVICE_NAME, &emp_fops);
	if (emp_major < 0) {
		printk(KERN_ERR "unable to register %s devs: %d\n",
				EMP_DEVICE_NAME,
				emp_major);
		ret = -1;
		goto err;
	}

	emp_class = class_create(THIS_MODULE, EMP_DEVICE_NAME);
	emp_class->dev_uevent = emp_uevent;

	err_dev = device_create(emp_class, NULL, MKDEV(emp_major, 0),
							NULL, EMP_DEVICE_NAME);
	printk(KERN_INFO "%s: register device at major %d\n", EMP_DEVICE_NAME,
			emp_major);

	return 0;

err:
	emp_procfs_exit();
	emp_kfree(emp_mm_arr);
	return ret;
}

/**
 * emp_exit - Exit EMP module
 *
 * Destroy EMP module structures
 */
static void __exit emp_exit(void)
{
#ifdef CONFIG_EMP_VM
	struct emp_mod emp_mod;
	memset(&emp_mod, 0, sizeof(struct emp_mod));
#endif /* CONFIG_EMP_VM */

	if (emp_mm_arr_len != 0) {
		printk(KERN_ERR "unable to unregister %s: %d\n",
				EMP_DEVICE_NAME,
				emp_major);
		return;
	}


	/* workaround code for preventing from system crash
	 * due to modules_exit while the emp is released  */
	mutex_lock(&emp_open_mutex);
	mutex_unlock(&emp_open_mutex);

	if (emp_mm_arr)
		emp_kfree(emp_mm_arr);
	dma_exit();

	device_destroy(emp_class, MKDEV(emp_major,0));
	class_destroy(emp_class);
	unregister_chrdev(emp_major, EMP_DEVICE_NAME);

#ifdef CONFIG_EMP_VM
	register_emp_mod(&emp_mod);
#endif /* CONFIG_EMP_VM */
	kernel_symbol_close();
	emp_procfs_exit();

	emp_debug_alloc_exit();
	printk(KERN_INFO "%s: unregister device at major %d\n", EMP_DEVICE_NAME,
			emp_major);
}

module_init(emp_init)
module_exit(emp_exit)
MODULE_LICENSE("GPL");
