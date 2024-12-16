#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#include "config.h"
#include "gpa.h"
#include "vm.h"
#include "reclaim.h"
#include "donor_mgmt.h"
#include "procfs.h"

struct proc_dir_entry *emp_proc_dir = NULL;
EXPORT_SYMBOL(emp_proc_dir);

/**************************************************/
/* helper functions                               */
/**************************************************/
#define ____concat2(a, b) a##b
#define ____concat3(a, b, c) a##b##c

#define __PAGES_TO_SIZE(p) (((size_t) (p)) << (PAGE_SHIFT))
#define __PAGES_TO_SIZE_VM(p, bvma) (((size_t) (p)) << (PAGE_SHIFT))
#define __SIZE_TO_PAGES(s) ((s) >> (PAGE_SHIFT))
#define __SIZE_TO_PAGES_VM(s, bvma) ((s) >> (PAGE_SHIFT))

static inline struct emp_mm *__get_emp_mm_by_file(struct file *file) {
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0))
	return PDE_DATA(file->f_inode);
#else
	return pde_data(file->f_inode);
#endif
}

#define CHECK_INPUT_LENGTH(length, min, max) do { \
	if ((length) < (min) || (length) > (max)) { \
		printk(KERN_ERR "%s: input is too %s\n", __func__, \
						(length) < (min) ? "short" : "long"); \
		return -EINVAL; \
	} \
} while(0)

static int __emp_proc_copy_from(char *buffer, ssize_t min, ssize_t max,
					const char __user *buf, size_t count) {
	if (count < min || count > max) {
		printk(KERN_ERR "input is too %s\n", count < min ? "short" : "long"); 
		return -EINVAL;
	}

	memset(buffer, 0, max);
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	else
		return 0;
}

#define EMP_PROC_COPY_FROM(buffer, min, max, buf, count) do { \
	int ____ret = __emp_proc_copy_from(buffer, min, max, buf, count); \
	if (____ret) return ____ret; \
} while(0)

static long __strtol(char *buf, int *err) {
	char *ptr = buf;
	char fmt[] = "%lX";
	long ret;

	if (*ptr == '0') {
		ptr++;
		if (*ptr == '\n' || *ptr == '\0')
			return 0;
		else if (*ptr == 'x') {
			fmt[2] = 'x';
			ptr++;
		} else {
			fmt[2] = 'o';
		}
	} else
		fmt[2] = 'd';

	if (sscanf(ptr, fmt, &ret) != 1)
		*err = -EINVAL;
	else
		*err = 0;
	
	return ret;
}

static ssize_t __size_read(struct file *file, char __user *buf,
					size_t count, loff_t *ppos, size_t val) {
	char buffer[PROC_BUF_SIZE];
	ssize_t len;

	memset(buffer, 0, PROC_BUF_SIZE);

	if (val >= (1UL<<GB_ORDER))
		len = snprintf(buffer, PROC_BUF_SIZE, "%ld GiB\n", val >> GB_ORDER);
	else if (val > (1UL<<MB_ORDER))
		len = snprintf(buffer, PROC_BUF_SIZE, "%ld MiB\n", val >> MB_ORDER);
	else if (val > (1UL<<KB_ORDER))
		len = snprintf(buffer, PROC_BUF_SIZE, "%ld KiB\n", val >> KB_ORDER);
	else
		len = snprintf(buffer, PROC_BUF_SIZE, "%ld B\n", val);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

/* This function actually DOES NOT write the value. 
   It just parses and returns the value. */
static ssize_t __size_write(struct file *file, const char __user *buf,
							size_t count, loff_t *ppos) {
	char buffer[PROC_BUF_SIZE];
	int end, order;
	ssize_t val;
	int err = 0;

	EMP_PROC_COPY_FROM(buffer, 1, PROC_BUF_SIZE, buf, count);

	end = strnlen(buffer, PROC_BUF_SIZE);

	if (end > 1 && buffer[end - 1] == '\n')
		buffer[--end] = '\0';

	if (end > 1 && (buffer[end - 1] == 'B' || buffer[end - 1] == 'b'))
		buffer[--end] = '\0';

	if (end > 1) {
		if (buffer[end - 1] == 'G' || buffer[end - 1] == 'g') {
			order = 30;
			end--;
		} else if (buffer[end - 1] == 'M' || buffer[end - 1] == 'm') {
			order = 20;
			end--;
		} else if (buffer[end - 1] == 'K' || buffer[end - 1] == 'k') {
			order = 10;
			end--;
		} else
			order = 0;
		buffer[end] = '\0';
	} else
		order = 0;

	val = __strtol(buffer, &err);
	if (err)
		return err;
	
	return (val << order);
}

static ssize_t __integer_read(struct file *file, char __user *buf,
					size_t count, loff_t *ppos, int val) {
	char buffer[PROC_BUF_SIZE];
	size_t len;
	len = snprintf(buffer, PROC_BUF_SIZE, "%d\n", val);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

// if min > max, don't check the range
static inline ssize_t __integer_write(struct file *file, const char __user *buf,
					size_t count, loff_t *ppos, int *val, int min, int max) {
	char buffer[PROC_BUF_SIZE];
	int temp;

	EMP_PROC_COPY_FROM(buffer, 1, PROC_BUF_SIZE, buf, count);

	sscanf(buffer, "%d", &temp);
	if (min <= max && (temp < min || temp > max)) {
		printk(KERN_ERR "input has wrong value %d (%d ~ %d can be accepted)\n", temp, min, max);
		return -EINVAL;
	}
	*val = temp;

	return count;
}

#ifdef CONFIG_EMP_DEBUG_ALLOC
static ssize_t __atomic64_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos, atomic64_t *val) {
	char buffer[PROC_BUF_SIZE];
	size_t len;
	len = snprintf(buffer, PROC_BUF_SIZE, "%lld\n", atomic64_read(val));
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}
#endif

/**************************************************/
/* global files                                   */
/**************************************************/

#define EMP_PROC_INITIAL_INTEGER_READ(x) \
	static ssize_t ____concat3(initial_, x, _read) \
					(struct file *file, char __user *buf, size_t count, loff_t *ppos) \
	{	\
		extern int ____concat2(initial_, x); \
		return __integer_read(file, buf, count, ppos, ____concat2(initial_, x)); \
	}

#define EMP_PROC_INITIAL_INTEGER_WRITE_RANGE(x, min, max) \
	static ssize_t ____concat3(initial_, x, _write) \
					(struct file *file, const char __user *buf, size_t count, loff_t *ppos) \
	{	\
		extern int ____concat2(initial_, x); \
		ssize_t ret = __integer_write(file, buf, count, ppos, \
							&____concat2(initial_, x), min, max); \
		if (ret >= 0) \
			printk(KERN_INFO "%s: %d\n", __stringify(____concat2(initial_, x)), \
											____concat2(initial_, x)); \
		return ret; \
	}

/* don't check the range */
#define EMP_PROC_INITIAL_INTEGER_WRITE(x) EMP_PROC_INITIAL_INTEGER_WRITE_RANGE(x, 1, 0)

#define EMP_PROC_INITIAL_BOOLEAN_READ(x) EMP_PROC_INITIAL_INTEGER_READ(x)
#define EMP_PROC_INITIAL_BOOLEAN_WRITE(x) EMP_PROC_INITIAL_INTEGER_WRITE_RANGE(x, 0, 1)

#define EMP_PROC_INITIAL_BOOLEAN(x) \
	EMP_PROC_INITIAL_BOOLEAN_READ(x) \
	EMP_PROC_INITIAL_BOOLEAN_WRITE(x)

#define EMP_PROC_INITIAL_INTEGER_RANGE(x, min, max) \
	EMP_PROC_INITIAL_INTEGER_READ(x) \
	EMP_PROC_INITIAL_INTEGER_WRITE_RANGE(x, min, max)

#define EMP_PROC_INITIAL_INTEGER(x) \
	EMP_PROC_INITIAL_INTEGER_READ(x) \
	EMP_PROC_INITIAL_INTEGER_WRITE(x)

#ifdef CONFIG_EMP_DEBUG
#define EMP_PROC_ATOMIC64_READ(x) \
	static ssize_t ____concat2(x, _read) \
		(struct file *file, char __user *buf, size_t count, loff_t *ppos) \
	{ \
		extern atomic64_t x; \
		return __atomic64_read(file, buf, count, ppos, &x); \
	}
#endif

static ssize_t initial_local_cache_size_read(struct file *file, char __user *buf,
							size_t count, loff_t *ppos)
{
	extern size_t initial_local_cache_pages;
	return __size_read(file, buf, count, ppos, 
						__PAGES_TO_SIZE(initial_local_cache_pages));
}

static ssize_t initial_local_cache_size_write(struct file *file, const char __user *buf,
						size_t count, loff_t *ppos)
{
	extern size_t initial_local_cache_pages;
	ssize_t local_cache_size;

	local_cache_size = __size_write(file, buf, count, ppos);
	if (local_cache_size < 0)
		return local_cache_size;

	if (local_cache_size < __PAGES_TO_SIZE(1)) {
		printk(KERN_ERR "ERROR: initial_local_cache_size should be larger than 0x%lx (subblock size)\n", 
					__PAGES_TO_SIZE(1));
		return -EINVAL;
	}

	printk("initial_local_page_size: 0x%lx\n", local_cache_size);
	initial_local_cache_pages = __SIZE_TO_PAGES(local_cache_size);

	return count;
}

#ifdef CONFIG_EMP_DEBUG_ALLOC
EMP_PROC_ATOMIC64_READ(emp_debug_alloc_size_aggr);
EMP_PROC_ATOMIC64_READ(emp_debug_alloc_size_max);
EMP_PROC_ATOMIC64_READ(emp_debug_alloc_size_alloc);
EMP_PROC_ATOMIC64_READ(emp_debug_alloc_size_free);
#endif

/**************************************************/
/* per-VM files - status and control              */
/**************************************************/

#define __EMP_PROC_VM_INTEGER_READ(name, var) \
	static ssize_t ____concat2(name, _read) \
					(struct file *file, char __user *buf, size_t count, loff_t *ppos) \
	{	\
		ssize_t ret = 0; \
		struct emp_mm *bvma = __get_emp_mm_by_file(file); \
		if (bvma == NULL) return 0; \
		rcu_read_lock(); \
		if (bvma->close == 0) \
			ret = __integer_read(file, buf, count, ppos, bvma->var); \
		rcu_read_unlock(); \
		return ret; \
	}

#define EMP_PROC_VM_INTEGER_READ(x) __EMP_PROC_VM_INTEGER_READ(x, x)
#define EMP_PROC_VM_CONFIG_INTEGER_READ(x) __EMP_PROC_VM_INTEGER_READ(x, config.x)

#define __EMP_PROC_VM_INTEGER_WRITE_RANGE(name, var, min, max) \
	static ssize_t ____concat2(name, _write) \
					(struct file *file, const char __user *buf, size_t count, loff_t *ppos) \
	{	\
		ssize_t ret = 0; \
		struct emp_mm *bvma = __get_emp_mm_by_file(file); \
		if (bvma == NULL) return 0; \
		rcu_read_lock(); \
		if (bvma->close == 0) { \
			ret = __integer_write(file, buf, count, ppos, \
							&(bvma->var), min, max); \
		} \
		rcu_read_unlock(); \
		if (ret > 0) \
			printk(KERN_INFO "%s: emp_id: %d value: %d\n", __stringify(name), \
											bvma->id, bvma->var); \
		return ret; \
	}

#define EMP_PROC_VM_INTEGER_WRITE(x) __EMP_PROC_VM_INTEGER_WRITE_RANGE(x, x, 1, 0)
#define EMP_PROC_VM_CONFIG_INTEGER_WRITE(x) __EMP_PROC_VM_INTEGER_WRITE_RANGE(x, config.x, 1, 0)

#define EMP_PROC_VM_CONFIG_BOOLEAN_READ(x) __EMP_PROC_VM_INTEGER_READ(x, config.x)
#define EMP_PROC_VM_CONFIG_BOOLEAN_WRITE(x) __EMP_PROC_VM_INTEGER_WRITE_RANGE(x, config.x, 0, 1)

#define EMP_PROC_VM_BOOLEAN_READ(x) __EMP_PROC_VM_INTEGER_READ(x, x)
#define EMP_PROC_VM_BOOLEAN_WRITE(x) __EMP_PROC_VM_INTEGER_WRITE_RANGE(x, x, 0, 1)

#define EMP_PROC_VM_CONFIG_BOOLEAN(x) \
		EMP_PROC_VM_CONFIG_BOOLEAN_READ(x) \
		EMP_PROC_VM_CONFIG_BOOLEAN_WRITE(x)

#define EMP_PROC_VM_BOOLEAN(x) \
		EMP_PROC_VM_BOOLEAN_READ(x) \
		EMP_PROC_VM_BOOLEAN_WRITE(x)

static ssize_t online_read(struct file *file, char __user *buf,
							size_t count, loff_t *ppos)
{
	ssize_t ret = 0;
	struct emp_mm *bvma = __get_emp_mm_by_file(file);
	if (bvma == NULL) return 0;
	rcu_read_lock();
	if (bvma->close == 0) 
		ret = __integer_read(file, buf, count, ppos, bvma->vmrs_len);
	rcu_read_unlock();
	return ret;
}

static ssize_t local_cache_size_read(struct file *file, char __user *buf,
							size_t count, loff_t *ppos)
{
	ssize_t ret = 0;
	struct emp_mm *bvma = __get_emp_mm_by_file(file);
	if (bvma == NULL) return 0;

	rcu_read_lock();
	if (bvma->close == 0) {
		ret = __size_read(file, buf, count, ppos, 
			__PAGES_TO_SIZE_VM(atomic_read(&bvma->ftm.local_cache_pages), bvma));
	}
	rcu_read_unlock();
	return ret;
}



/**************************************************/
/* proc entries                                   */
/**************************************************/

#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0))
#define emp_proc_entry_initial_rw(x) \
	{ \
		.name = __stringify(____concat2(initial_, x)), \
		.mode = 0664, \
		.ops = { \
					.read =  ____concat3(initial_, x, _read), \
					.write = ____concat3(initial_, x, _write), \
					.llseek = default_llseek, \
		}, \
	}

#define emp_proc_entry_rw(x) \
	{ \
		.name = __stringify(x), \
		.mode = 0664, \
		.ops = { \
					.read =  ____concat2(x, _read), \
					.write = ____concat2(x, _write), \
					.llseek = default_llseek, \
		}, \
	}

#define emp_proc_entry_ro(x) \
	{ \
		.name = __stringify(x), \
		.mode = 0444, \
		.ops = { \
					.read =  ____concat2(x, _read), \
					.llseek = default_llseek, \
		}, \
	}

#define emp_proc_entry_wo(x) \
	{ \
		.name = __stringify(x), \
		.mode = 0222, \
		.ops = { \
					.write = ____concat2(x, _write), \
					.llseek = default_llseek, \
		}, \
	}
#else
#define emp_proc_entry_initial_rw(x) \
	{ \
		.name = __stringify(____concat2(initial_, x)), \
		.mode = 0664, \
		.ops = { \
					.proc_read =  ____concat3(initial_, x, _read), \
					.proc_write = ____concat3(initial_, x, _write), \
					.proc_lseek = default_llseek, \
		}, \
	}

#define emp_proc_entry_rw(x) \
	{ \
		.name = __stringify(x), \
		.mode = 0664, \
		.ops = { \
					.proc_read =  ____concat2(x, _read), \
					.proc_write = ____concat2(x, _write), \
					.proc_lseek = default_llseek, \
		}, \
	}

#define emp_proc_entry_ro(x) \
	{ \
		.name = __stringify(x), \
		.mode = 0444, \
		.ops = { \
					.proc_read =  ____concat2(x, _read), \
					.proc_lseek = default_llseek, \
		}, \
	}

#define emp_proc_entry_wo(x) \
	{ \
		.name = __stringify(x), \
		.mode = 0222, \
		.ops = { \
					.proc_write = ____concat2(x, _write), \
					.proc_lseek = default_llseek, \
		}, \
	}
#endif

#define emp_proc_entry_END { .name = NULL, }

static struct emp_proc_entry emp_proc_global[] = {
	emp_proc_entry_initial_rw(local_cache_size),
#ifdef CONFIG_EMP_DEBUG_ALLOC
	emp_proc_entry_ro(emp_debug_alloc_size_aggr),
	emp_proc_entry_ro(emp_debug_alloc_size_max),
	emp_proc_entry_ro(emp_debug_alloc_size_alloc),
	emp_proc_entry_ro(emp_debug_alloc_size_free),
#endif
	emp_proc_entry_END,
};

static struct emp_proc_entry emp_proc_vm[] = {
	emp_proc_entry_ro(online),
	emp_proc_entry_ro(local_cache_size),
	emp_proc_entry_END,
};


static inline int emp_procfs_reg(struct emp_proc_entry *list, 
						  struct proc_dir_entry *pde, 
						  struct emp_mm *bvma) 
{
	int i = 0;

	while (list[i].name != NULL) {
		if (proc_create_data(list[i].name, list[i].mode,
							pde, &list[i].ops, bvma) == NULL)
			return -1;
		i++;
	}

	return 0;
}

void emp_procfs_del(struct emp_mm *bvma)
{
	if (bvma->emp_proc_dir) {
		proc_remove(bvma->emp_proc_dir);
		bvma->emp_proc_dir = NULL;
	}
}

int emp_procfs_add(struct emp_mm *bvma, int id)
{
	char dirname[PROC_NAME_SIZE];

	if (bvma->emp_proc_dir)
		return -1;

	if (emp_proc_dir == NULL)
		return -1;

	snprintf(dirname, PROC_NAME_SIZE, "%d", id);
	bvma->emp_proc_dir = proc_mkdir(dirname, emp_proc_dir);
	if (!bvma->emp_proc_dir)	
		goto err;

	if (emp_procfs_reg(emp_proc_vm, bvma->emp_proc_dir, bvma))
		goto err;


	return 0;

err:
	emp_procfs_del(bvma);
	return -1;
}

void emp_procfs_exit(void)
{
	if (emp_proc_dir) {
		proc_remove(emp_proc_dir);
		emp_proc_dir = NULL;
	}
}

int emp_procfs_init(void)
{
	
	emp_proc_dir = proc_mkdir(EMP_DEVICE_NAME, NULL);
	if (!emp_proc_dir)
		return -ENOMEM;

	if (emp_procfs_reg(emp_proc_global, emp_proc_dir, NULL)) {
		emp_procfs_exit();
		return -1;
	}

	return 0;
}
