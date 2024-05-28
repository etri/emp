#ifndef __BLK_PREFETCH_H__
#define __BLK_PREFETCH_H__

#include "vm.h"

long emp_blk_prefetch(struct emp_mm *emm, unsigned long addr, unsigned long size);
#endif /* __BLK_PREFETCH_H__ */
