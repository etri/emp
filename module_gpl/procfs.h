#ifndef EMP_PROCFS_H
#define EMP_PROCFS_H

#include <linux/proc_fs.h>
#include "config.h"

extern struct proc_dir_entry *emp_proc_dir;

#define PROC_BUF_SIZE 64
#define PROC_NAME_SIZE 64

struct emp_proc_entry {
	char *name;
	umode_t mode;
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 0)) \
	|| (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0))
	struct file_operations ops;
#else
	struct proc_ops ops;
#endif
};

int emp_procfs_add(struct emp_mm *bvma, int id);
void emp_procfs_del(struct emp_mm *bvma);
int emp_procfs_init(void);
void emp_procfs_exit(void);

#endif /* EMP_PROCFS_H */
