#ifndef __EMP_MADVISE_H__
#define __EMP_MADVISE_H__

#include "vm.h"

long emp_blk_prefetch(struct emp_mm *emm, unsigned long addr, unsigned long size);
#endif /* __EMP_MADVISE_H__ */
