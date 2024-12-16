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

#ifdef CONFIG_EMP_BLOCK
static ssize_t initial_block_size_read(struct file *file, char __user *buf,
											size_t count, loff_t *ppos) 
{
	extern int initial_block_order;
	return __size_read(file, buf, count, ppos, __PAGES_TO_SIZE(1) << initial_block_order);
}

static ssize_t initial_block_size_write(struct file *file, const char __user *buf,
										size_t count, loff_t *ppos)
{
	extern int initial_block_order;
	extern int initial_subblock_order;
	size_t block_size;
	int block_order;

	block_size = __size_write(file, buf, count, ppos);

	if (block_size < 0)
		return block_size;
	
	block_order = get_order(block_size);
	// temporally huge page is not supported
	if (block_order < 0 || block_order > HPAGE_PMD_ORDER) {
		printk(KERN_ERR "ERROR: requested block size is not applied. size: 0x%lx order: %d\n",
				block_size, block_order);
		return -EINVAL;
	}
	// smaller than DMA size
	else if (block_order < initial_subblock_order) {
		printk(KERN_ERR "ERROR: requested subblock size(0x%lx) is not applied since it is smaller than initial subblock size(0x%lx)\n",
				block_size, 1L << initial_subblock_order);
		return -EINVAL;
	}
	
	printk("initial_block_order: %d (size: 0x%lx)\n", block_order, block_size);
	initial_block_order = block_order;

	return count;
}

static ssize_t initial_subblock_size_read(struct file *file, char __user *buf,
											size_t count, loff_t *ppos) 
{
	extern int initial_subblock_order;
	return __size_read(file, buf, count, ppos, __PAGES_TO_SIZE(1) << initial_subblock_order);
}

static ssize_t initial_subblock_size_write(struct file *file, const char __user *buf,
										size_t count, loff_t *ppos)
{
	extern int initial_subblock_order;
	size_t subblock_size;
	int subblock_order;

	subblock_size = __size_write(file, buf, count, ppos);

	if (subblock_size < 0)
		return subblock_size;
	
	subblock_order = get_order(subblock_size);
	// temporally huge page is not supported
	if (subblock_order < 0 || subblock_order > HPAGE_PMD_ORDER) {
		printk(KERN_ERR "ERROR: requested subblock size is not applied. size: 0x%lx order: %d\n",
				subblock_size, subblock_order);
		return -EINVAL;
	}
	// smaller than DMA size
	else if (subblock_order > DMA_OPERATION_MAX_ORDER) {
		printk(KERN_ERR "ERROR: requested subblock size is not applied. size: 0x%lx order: %d\n",
				subblock_size, subblock_order);
		return -EINVAL;
	}
	
	printk("initial_subblock_order: %d (size: 0x%lx)\n", subblock_order, subblock_size);
	initial_subblock_order = subblock_order;

	return count;
}

EMP_PROC_INITIAL_BOOLEAN(use_compound_page)
EMP_PROC_INITIAL_BOOLEAN(critical_subblock_first)
EMP_PROC_INITIAL_BOOLEAN_READ(critical_page_first)
static ssize_t initial_critical_page_first_write(struct file *file, const char __user *buf,
											size_t count, loff_t *ppos) {
	extern int initial_critical_page_first;
	extern int initial_critical_subblock_first;
#ifdef CONFIG_EMP_RDMA
	extern int initial_mark_empty_page;
#endif
	int critical_page_first = initial_critical_page_first;
	ssize_t ret = __integer_write(file, buf, count, ppos,
							&critical_page_first, 0, 1);

	if (ret < 0) return ret;

#ifdef CONFIG_EMP_RDMA
	if ((critical_page_first == 1) &&
			(initial_critical_subblock_first == 0 || initial_mark_empty_page == 0)) {
		printk(KERN_ERR "critical_subblock_first and mark_empty_page must be enabled for critical_page_first\n");
		return -EINVAL;
	}
#else
	printk(KERN_ERR "critical_page_first is currently supported only on RDMA\n");
	return -EINVAL;
#endif

	initial_critical_page_first = critical_page_first;
	printk(KERN_INFO "initial_critical_page_first: %d\n", initial_critical_page_first);

	return ret;
}

EMP_PROC_INITIAL_BOOLEAN_READ(enable_transition_csf)
static ssize_t initial_enable_transition_csf_write(struct file *file, const char __user *buf,
											size_t count, loff_t *ppos) {
	extern int initial_enable_transition_csf;
	extern int initial_critical_page_first;
	extern int initial_critical_subblock_first;
	int enable_transition_csf = initial_enable_transition_csf;
	ssize_t ret = __integer_write(file, buf, count, ppos,
							&enable_transition_csf, 0, 1);

	if (ret < 0) return ret;

	if ((enable_transition_csf == 1) &&
			(initial_critical_subblock_first == 0 && initial_critical_page_first == 0)) {
		printk(KERN_ERR "critical_subblock_first or critical_page_first must be enabled for enable_transition_csf\n");
		return -EINVAL;
	}

	initial_enable_transition_csf = enable_transition_csf;
	printk(KERN_INFO "initial_enable_transition_csf: %d\n", initial_enable_transition_csf);

	return ret;
}

EMP_PROC_INITIAL_BOOLEAN(mark_empty_page)
EMP_PROC_INITIAL_BOOLEAN(mem_poll)
#endif
#ifdef CONFIG_EMP_OPT
EMP_PROC_INITIAL_BOOLEAN(eval_media)
#endif
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

#ifdef CONFIG_EMP_BLOCK
EMP_PROC_VM_CONFIG_INTEGER_READ(block_order)
EMP_PROC_VM_CONFIG_INTEGER_READ(subblock_order)
EMP_PROC_VM_CONFIG_BOOLEAN(critical_subblock_first)

EMP_PROC_VM_CONFIG_BOOLEAN_READ(critical_page_first);
static ssize_t critical_page_first_write(struct file *file, const char __user *buf,
									size_t count, loff_t *ppos)
{
	ssize_t ret;
	int critical_page_first;
	struct emp_mm *bvma = __get_emp_mm_by_file(file);
	if (bvma == NULL) return 0;
	critical_page_first = bvma->config.critical_page_first;

	ret = __integer_write(file, buf, count, ppos,
						&critical_page_first, 0, 1);
	if (ret < 0) return ret;

#ifdef CONFIG_EMP_RDMA
	if ((critical_page_first == 1) &&
			(bvma->config.critical_subblock_first == 0 ||
			 bvma->config.mark_empty_page == 0)) {
		printk(KERN_ERR "ERROR: [emp_id: %d] critical_subblock_first and "
				"mark_empty_page must be enabled for critical_page_first\n",
						bvma->id);
		return -EINVAL;
	}
#else
	printk(KERN_ERR "ERROR: [emp_id: %d] critical_page_first is currently supported only on RDMA\n",
			bvma->id);
	return -EINVAL;
#endif

	bvma->config.critical_page_first = critical_page_first;
	printk(KERN_INFO "critical_page_first: emp_id: %d value: %d\n", bvma->id, critical_page_first);
	return ret;
}

EMP_PROC_VM_CONFIG_BOOLEAN_READ(enable_transition_csf);
static ssize_t enable_transition_csf_write(struct file *file, const char __user *buf,
									size_t count, loff_t *ppos)
{
	ssize_t ret;
	int enable_transition_csf;
	struct emp_mm *bvma = __get_emp_mm_by_file(file);
	if (bvma == NULL) return 0;
	enable_transition_csf = bvma->config.enable_transition_csf;

	ret = __integer_write(file, buf, count, ppos,
						&enable_transition_csf, 0, 1);
	if (ret < 0) return ret;

	if ((enable_transition_csf == 1) &&
			(bvma->config.critical_subblock_first == 0 &&
			 bvma->config.critical_page_first == 0)) {
		printk(KERN_ERR "ERROR: [emp_id: %d] critical_subblock_first or "
				"critical_page_first must be enabled for enable_transition_csf\n",
						bvma->id);
		return -EINVAL;
	}

	bvma->config.enable_transition_csf = enable_transition_csf;
	printk(KERN_INFO "enable_transition_csf: emp_id: %d value: %d\n", bvma->id, enable_transition_csf);
	return ret;
}

EMP_PROC_VM_CONFIG_BOOLEAN(mark_empty_page);
EMP_PROC_VM_CONFIG_BOOLEAN(mem_poll);
EMP_PROC_VM_CONFIG_BOOLEAN_READ(use_compound_page);
#endif
#ifdef CONFIG_EMP_OPT
EMP_PROC_VM_CONFIG_BOOLEAN(next_pt_premapping)
EMP_PROC_VM_CONFIG_BOOLEAN_READ(eval_media);
#endif
#ifdef CONFIG_EMP_STAT
EMP_PROC_VM_CONFIG_BOOLEAN(reset_after_read)
#endif
#ifdef CONFIG_EMP_OPT
#ifdef CONFIG_EMP_BLOCKDEV
static inline ssize_t __read_poll_read(struct file *file, char __user *buf,
								size_t count, loff_t *ppos, struct emp_mm *bvma)
{
	size_t buf_size = PROC_BUF_SIZE * bvma->mrs.memregs_len;	
	char buffer[buf_size];
	ssize_t len = 0;
	int i, rcu_index;
	struct memreg *m;

	rcu_read_lock();
	if (bvma->close) {
		rcu_read_unlock();
		return 0;
	}
	rcu_index = srcu_read_lock(&bvma->srcu);
	rcu_read_unlock();
	for (i = 0; i < bvma->mrs.memregs_len; i++) {
		m = bvma->mrs.memregs[i];
		if (m == NULL)
			continue;

		len += snprintf(buffer + len, buf_size - len,
					"donor[%d] %s\n", i,
					m->conn->read_command_flag ? "enable" : "disable");
	}
	srcu_read_unlock(&bvma->srcu, rcu_index);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}
#endif /* CONFIG_EMP_BLOCKDEV */

static ssize_t read_poll_read(struct file *file, char __user *buf,
								size_t count, loff_t *ppos)
{
#ifdef CONFIG_EMP_BLOCKDEV
	ssize_t len;
	struct emp_mm *bvma = __get_emp_mm_by_file(file);
	if (bvma == NULL) return 0;

	spin_lock(&bvma->mrs.memregs_lock);
	len = __read_poll_read(file, buf, count, ppos, bvma);
	spin_unlock(&bvma->mrs.memregs_lock);

	return len;
#else /* !CONFIG_EMP_BLOCKDEV */
	return 0;
#endif /* !CONFIG_EMP_BLOCKDEV */
}

static ssize_t read_poll_write(struct file *file, const char __user *buf,
								size_t count, loff_t *ppos)
{
#ifdef CONFIG_EMP_BLOCKDEV
	char buffer[PROC_BUF_SIZE * 2];
	unsigned int mrid, flag;
	struct memreg *m;
	struct request_queue *q;
	struct connection *conn;
	ssize_t ret = count;
	
	struct emp_mm *bvma = __get_emp_mm_by_file(file);
	if (bvma == NULL) return 0;

	EMP_PROC_COPY_FROM(buffer, 3, PROC_BUF_SIZE * 2, buf, count);

	if (sscanf(buffer, "%d %d\n", &mrid, &flag) != 2)
		return -EINVAL;

	spin_lock(&bvma->mrs.memregs_lock);
	if (mrid >= bvma->mrs.memregs_len) {
		ret = -ERANGE;
		goto out;
	}

	m = bvma->mrs.memregs[mrid];
	if (!m || !m->conn) {
		ret = -ENODEV;
		goto out;
	}

	conn = m->conn;
	if (!conn->bdev || atomic_read(&conn->refcount) == 0) {
		ret = -EIO;
		goto out;
	}

	q = conn->bdev->bd_disk->queue;
	if (flag && test_bit(QUEUE_FLAG_POLL, &q->queue_flags)) {
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0))
		m->conn->read_command_flag = REQ_HIPRI;
#else
		m->conn->read_command_flag = REQ_POLLED;
#endif
	} else {
		m->conn->read_command_flag = 0;
	}
	printk(KERN_INFO "read_poll: emp_id: %d mrid: %d value: %s\n",
			bvma->id, mrid,
			m->conn->read_command_flag ? "enable" : "disable");
out:
	spin_unlock(&bvma->mrs.memregs_lock);
	return ret;
#else /* !CONFIG_EMP_BLOCKDEV */
	return 0;
#endif /* !CONFIG_EMP_BLOCKDEV */
}
#endif /* CONFIG_EMP_OPT */
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

#ifdef CONFIG_EMP_RDMA
static inline ssize_t __donor_info_show(struct file *file, char __user *buf,
							size_t count, loff_t *ppos, struct emp_mm *bvma)
{
	size_t buf_size = PROC_BUF_SIZE * bvma->mrs.memregs_len * 4;
	char buffer[buf_size];
	ssize_t len = 0;
	int j;
	struct memreg *m;
	size_t free;
	
	rcu_read_lock();
	if (bvma->close)
		goto out;
	for (j = 0; j < bvma->mrs.memregs_len; j++) {
		m = bvma->mrs.memregs[j];
		if (m == NULL)
			continue;

		free = m->size - atomic64_read(&m->alloc_len);
		len += snprintf(buffer + len, buf_size - len,
				"donor[%d] %pI4 "
				"port:%d pid:%d "
				"size: %lld MiB "
				"free: %ld MiB\n",
				j, &m->addr, m->port,
				m->conn->dpid,
				PAGE_TO_MB(m->size), 
				PAGE_TO_MB(free));
	}
out:
	rcu_read_unlock();
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t donor_info_read(struct file *file, char __user *buf,
						size_t count, loff_t *ppos)
{
	ssize_t len;
	struct emp_mm *bvma;

	bvma = __get_emp_mm_by_file(file);
	if (bvma == NULL) return 0;

	spin_lock(&bvma->mrs.memregs_lock);
	len = __donor_info_show(file, buf, count, ppos, bvma);
	spin_unlock(&bvma->mrs.memregs_lock);

	return len;
}
#endif /* CONFIG_EMP_RDMA */

#ifdef CONFIG_EMP_STAT
/**************************************************/
/* per-VM files - stat                            */
/**************************************************/

#define __EMP_PROC_STAT_READ_PER(type, name, var) \
	static inline ssize_t ____concat3(__, name, _read) \
						(struct file *file, char __user *buf, \
							size_t count, loff_t *ppos, struct emp_mm *bvma) \
	{ \
		size_t buf_size = PROC_BUF_SIZE * (IO_THREAD_MAX + KVM_THREAD_MAX + 4); \
		char *buffer = NULL; \
		ssize_t len = 0; \
		u64 val, sum = 0; \
		int j, srcu_index; \
		ssize_t ret; \
	\
		buffer = emp_kmalloc(buf_size, GFP_KERNEL); \
                if(buffer == NULL) return 0; \
        \
		rcu_read_lock(); \
		if (bvma->close) { \
			rcu_read_unlock(); \
			return 0; \
		} \
		srcu_index = srcu_read_lock(&bvma->srcu); \
		rcu_read_unlock(); \
		FOR_EACH_##type(bvma, j) { \
			val = (emp_get_vcpu_from_id(bvma, j))->stat.var; \
			len += snprintf(buffer+len, buf_size-len, "%lld\t", val); \
			sum += val; \
			if (bvma->config.reset_after_read) \
				(emp_get_vcpu_from_id(bvma, j))->stat.var = 0; \
		} \
		len += snprintf(buffer+len, buf_size-len, "sum: %lld\n", sum); \
		ret = simple_read_from_buffer(buf, count, ppos, buffer, len); \
	\
		srcu_read_unlock(&bvma->srcu, srcu_index); \
		emp_kfree(buffer); \
		return ret; \
	} \
	\
	static ssize_t ____concat2(name, _read) \
			(struct file *file, char __user *buf, size_t count, loff_t *ppos) \
	{ \
		struct emp_mm *bvma = __get_emp_mm_by_file(file); \
		if (bvma == NULL) return 0; \
		return ____concat3(__, name, _read)(file, buf, count, ppos, bvma); \
	}

#define __EMP_PROC_STAT_WRITE_PER(type, name, var) \
	static ssize_t ____concat2(name, _write) \
				(struct file *file, const char __user *buf, size_t count, loff_t *ppos) \
	{ \
		int j, srcu_index; \
		struct emp_mm *bvma = __get_emp_mm_by_file(file); \
		if (bvma == NULL) return 0; \
	\
		rcu_read_lock(); \
		if (bvma->close) { \
			rcu_read_unlock(); \
			return 0; \
		} \
		srcu_index = srcu_read_lock(&bvma->srcu); \
		rcu_read_unlock(); \
		FOR_EACH_##type(bvma, j) \
			(emp_get_vcpu_from_id(bvma, j))->stat.var = 0; \
		srcu_read_unlock(&bvma->srcu, srcu_index); \
	\
		return count; \
	}

#define __EMP_PROC_STAT_READ_VM(name, var) \
	static ssize_t ____concat2(name, _read) \
		(struct file *file, char __user *buf, size_t count, loff_t *ppos) \
	{ \
		char buffer[PROC_BUF_SIZE]; \
		ssize_t len = 0; \
		struct emp_mm *bvma = __get_emp_mm_by_file(file); \
		if (bvma == NULL) return 0; \
		rcu_read_lock(); \
		if (bvma->close == 0) { \
		len = snprintf(buffer, PROC_BUF_SIZE, "%lld\n", bvma->stat.var); \
		if (bvma->config.reset_after_read) \
			bvma->stat.var = 0; \
		} \
		rcu_read_unlock(); \
	\
		return simple_read_from_buffer(buf, count, ppos, buffer, len); \
	}

#define __EMP_PROC_STAT_WRITE_VM(name, var) \
	static ssize_t ____concat2(name, _write) \
		(struct file *file, const char __user *buf, size_t count, loff_t *ppos) \
	{ \
		struct emp_mm *bvma = __get_emp_mm_by_file(file); \
		if (bvma == NULL) return 0; \
		rcu_read_lock(); \
		if (bvma->close == 0) { \
			bvma->stat.var = 0; \
		} \
		rcu_read_unlock(); \
		return count; \
	}

#define __EMP_PROC_STAT_PER_VCPU(name, var) \
		__EMP_PROC_STAT_READ_PER(VCPU, name, var) \
		__EMP_PROC_STAT_WRITE_PER(VCPU, name, var) \

#define __EMP_PROC_STAT_PER_IOTHREAD(name, var) \
		__EMP_PROC_STAT_READ_PER(IOTHREAD, name, var) \
		__EMP_PROC_STAT_WRITE_PER(IOTHREAD, name, var) \

#define __EMP_PROC_STAT_PER_KVM_THREAD(name, var) \
		__EMP_PROC_STAT_READ_PER(KVM_THREAD, name, var) \
		__EMP_PROC_STAT_WRITE_PER(KVM_THREAD, name, var) \

#define __EMP_PROC_STAT_VM(name, var) \
		__EMP_PROC_STAT_READ_VM(name, var) \
		__EMP_PROC_STAT_WRITE_VM(name, var) \

#define EMP_PROC_STAT_PER_VCPU(x) __EMP_PROC_STAT_PER_VCPU(x, x)
#define EMP_PROC_STAT_VM(x) __EMP_PROC_STAT_VM(x, x)

/* for all vcpus (io threads + pure vcpu threads) */
EMP_PROC_STAT_PER_VCPU(vma_fault)
/* for io threads (qemu) */
__EMP_PROC_STAT_PER_IOTHREAD(hva_local_fault, local_fault)
__EMP_PROC_STAT_PER_IOTHREAD(hva_remote_fault, remote_fault)
/* for pure vcpu threads */
__EMP_PROC_STAT_PER_KVM_THREAD(gpa_local_fault, local_fault)
__EMP_PROC_STAT_PER_KVM_THREAD(gpa_remote_fault, remote_fault)

static ssize_t donor_reqs_read(struct file *file, char __user *buf,
									size_t count, loff_t *ppos)
{
	char buffer[PROC_BUF_SIZE * 4];
	ssize_t len = 0;
	struct emp_mm *bvma;

	bvma = __get_emp_mm_by_file(file);
	if (bvma == NULL) return 0;
	if (bvma->vmrs_len == 0) return 0;

	rcu_read_lock();
	if (bvma->close == 0) {
		len = snprintf(buffer, PROC_BUF_SIZE * 4, 
				"%d %d %d %d\n",
				atomic_read(&bvma->stat.read_reqs),
				atomic_read(&bvma->stat.read_comp),
				atomic_read(&bvma->stat.write_reqs),
				atomic_read(&bvma->stat.write_comp));
	}
	rcu_read_unlock();

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

__EMP_PROC_STAT_VM(reclaim, recl_count)
__EMP_PROC_STAT_VM(post_write, post_write_count)
__EMP_PROC_STAT_VM(post_read, post_read_count)
__EMP_PROC_STAT_VM(stale_page, stale_page_count)
EMP_PROC_STAT_VM(remote_tlb_flush)
EMP_PROC_STAT_VM(remote_tlb_flush_no_ipi)
EMP_PROC_STAT_VM(remote_tlb_flush_force)
EMP_PROC_STAT_VM(io_read_pages)
EMP_PROC_STAT_VM(csf_fault)
EMP_PROC_STAT_VM(csf_useful)
EMP_PROC_STAT_VM(cpf_to_csf_transition)
EMP_PROC_STAT_VM(post_read_mempoll)
__EMP_PROC_STAT_VM(fsync, fsync_count)
#endif /* CONFIG_EMP_STAT */

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
#ifdef CONFIG_EMP_BLOCK
	emp_proc_entry_initial_rw(block_size),
	emp_proc_entry_initial_rw(subblock_size),
	emp_proc_entry_initial_rw(use_compound_page),
	emp_proc_entry_initial_rw(critical_subblock_first),
	emp_proc_entry_initial_rw(critical_page_first),
	emp_proc_entry_initial_rw(enable_transition_csf),
	emp_proc_entry_initial_rw(mark_empty_page),
	emp_proc_entry_initial_rw(mem_poll),
#endif
#ifdef CONFIG_EMP_OPT
	emp_proc_entry_initial_rw(eval_media),
#endif
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
#ifdef CONFIG_EMP_BLOCK
	emp_proc_entry_ro(block_order),
	emp_proc_entry_ro(subblock_order),
	emp_proc_entry_rw(critical_subblock_first),
	emp_proc_entry_rw(critical_page_first),
	emp_proc_entry_rw(enable_transition_csf),
	emp_proc_entry_rw(mark_empty_page),
	emp_proc_entry_rw(mem_poll),
	emp_proc_entry_ro(use_compound_page),
#endif
#ifdef CONFIG_EMP_OPT
	emp_proc_entry_rw(next_pt_premapping),
	emp_proc_entry_ro(eval_media),
	emp_proc_entry_rw(read_poll),
#endif
#ifdef CONFIG_EMP_STAT
	emp_proc_entry_rw(reset_after_read),
#endif
	emp_proc_entry_ro(local_cache_size),
#ifdef CONFIG_EMP_RDMA
	emp_proc_entry_ro(donor_info),
#endif
	emp_proc_entry_END,
};

#ifdef CONFIG_EMP_STAT
static struct emp_proc_entry emp_proc_stat[] = {
	emp_proc_entry_rw(vma_fault),
	emp_proc_entry_rw(hva_local_fault),
	emp_proc_entry_rw(hva_remote_fault),
	emp_proc_entry_rw(gpa_local_fault),
	emp_proc_entry_rw(gpa_remote_fault),
	emp_proc_entry_ro(donor_reqs),
	emp_proc_entry_rw(reclaim),
	emp_proc_entry_rw(post_write),
	emp_proc_entry_rw(post_read),
	emp_proc_entry_rw(stale_page),
	emp_proc_entry_rw(remote_tlb_flush),
	emp_proc_entry_rw(remote_tlb_flush_no_ipi),
	emp_proc_entry_rw(remote_tlb_flush_force),
	emp_proc_entry_rw(io_read_pages),
	emp_proc_entry_rw(csf_fault),
	emp_proc_entry_rw(csf_useful),
	emp_proc_entry_rw(cpf_to_csf_transition),
	emp_proc_entry_rw(post_read_mempoll),
	emp_proc_entry_rw(fsync),
	emp_proc_entry_END,
};
#endif

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
#ifdef CONFIG_EMP_STAT
	if (bvma->emp_stat_dir) {
		proc_remove(bvma->emp_stat_dir);
		bvma->emp_stat_dir = NULL;
	}
#endif
	if (bvma->emp_proc_dir) {
		proc_remove(bvma->emp_proc_dir);
		bvma->emp_proc_dir = NULL;
	}
}

int emp_procfs_add(struct emp_mm *bvma, int id)
{
	char dirname[PROC_NAME_SIZE];

#ifdef CONFIG_EMP_STAT
	if (bvma->emp_proc_dir || bvma->emp_stat_dir)
		return -1;
#else
	if (bvma->emp_proc_dir)
		return -1;
#endif

	if (emp_proc_dir == NULL)
		return -1;

	snprintf(dirname, PROC_NAME_SIZE, "%d", id);
	bvma->emp_proc_dir = proc_mkdir(dirname, emp_proc_dir);
	if (!bvma->emp_proc_dir)	
		goto err;

	if (emp_procfs_reg(emp_proc_vm, bvma->emp_proc_dir, bvma))
		goto err;


#ifdef CONFIG_EMP_STAT
	bvma->emp_stat_dir = proc_mkdir("stat", bvma->emp_proc_dir);
	if (!bvma->emp_stat_dir)	
		goto err;
	
	if (emp_procfs_reg(emp_proc_stat, bvma->emp_stat_dir, bvma))
		goto err;
#endif
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

