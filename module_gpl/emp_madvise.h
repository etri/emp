#ifndef __EMP_MADVISE_H__
#define __EMP_MADVISE_H__

#include "vm.h"

long emp_blk_prefetch(struct emp_mm *emm, unsigned long addr, unsigned long size);
long emp_blk_move_to_inactive(struct emp_mm *emm, unsigned long addr, long __size);
long emp_madv_pin(struct emp_mm *emm, unsigned long addr, long size);
long emp_madv_unpin(struct emp_mm *emm, unsigned long addr, long size);
#endif /* __EMP_MADVISE_H__ */
