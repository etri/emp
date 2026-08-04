#ifndef __EMP_MADVISE_H__
#define __EMP_MADVISE_H__

#include "vm.h"

long emp_blk_prefetch(struct emp_mm *emm, unsigned long addr, unsigned long size);
long emp_blk_move_to_inactive(struct emp_mm *emm, unsigned long addr, long __size);
long emp_madv_pin(struct emp_mm *emm, unsigned long addr, long size);
long emp_madv_unpin(struct emp_mm *emm, unsigned long addr, long size);
#ifdef CONFIG_EMP_USER
long emp_madv_set_fork_policy(struct emp_mm *emm, unsigned long addr, long size,
				enum emp_fork_policy policy);
#endif
#endif /* __EMP_MADVISE_H__ */
