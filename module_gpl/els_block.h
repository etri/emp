#ifndef __ELSBLOCK_H__
#define __ELSBLOCK_H__

#include "../module_gpl/gpa.h"
#include "../module_gpl/vm.h"
#include "../module_gpl/block.h"
#include "vm.h"

#define ELS_REDUCE_THRESHOLD_ORDER (2)

#ifdef CONFIG_EMP_ELASTIC_BLOCK
#define BLOCK_NO_FETCH	(1 << 0)
#define BLOCK_FETCH_IN_PROGRESS (1 << 1)
#define BLOCK_READ_ONLY (1 << 2)
#define BLOCK_HVA_FAULT (1 << 3)
#define BLOCK_GPA_FAULT (1 << 4)
#define BLOCK_STALE     (1 << 6)

#define els_stretch_flag_add(flag, add) do { \
	(flag) |= (add); \
} while (0)
#else /* !CONFIG_EMP_ELASTIC_BLOCK */
#define els_stretch_flag_add(flag, add) do {} while (0)
#endif /* !CONFIG_EMP_ELASTIC_BLOCK */

bool els_stretch_rep(struct emp_mm *, struct vcpu_var *, struct emp_gpa *, u32, int);
bool els_reduce(struct emp_mm *,struct emp_gpa *, struct emp_gpa *hs[]);
bool els_tryreduce_complete(struct emp_mm *, struct emp_gpa *, struct emp_gpa *hs[]);
void els_init(struct emp_mm *, bool);

#endif /* __ELSBLOCK_H__ */
