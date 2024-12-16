#ifndef __DONOR_MGMT_H__
#define __DONOR_MGMT_H__

#include "remote_page.h"

/**
 * alloc_mrid - Assign a mr ID to the memory register
 * @param bvma bvma data structure
 * @param mr memory register
 *
 * @return memory register ID
 */
static inline u8 alloc_mrid(struct emp_mm *bvma, struct memreg *mr)
{
	int i, mrid = -1;

	spin_lock(&bvma->mrs.memregs_lock);
	for (i = 0; i < MAX_MRID; i++) {
		if (bvma->mrs.memregs[i] == NULL) {
			bvma->mrs.memregs[i] = mr;
			mrid = i;
			bvma->mrs.memregs_len++;
			if (mrid >= bvma->mrs.memregs_last)
				bvma->mrs.memregs_last = mrid + 1;
			break;
		}
	}
	spin_unlock(&bvma->mrs.memregs_lock);

	return mrid;
}

static inline void __reduce_memregs_last(struct emp_mm *bvma)
{
	while (bvma->mrs.memregs_last > 0
			&& bvma->mrs.memregs[bvma->mrs.memregs_last - 1] == NULL)
		bvma->mrs.memregs_last--;
}

/**
 * free_mrid - Unset a mrid
 * @param bvma bvma data structure
 * @param mrid memory register ID
 * @param locked is it locked?
 */
static inline struct memreg *
free_mrid(struct emp_mm *bvma, u8 mrid, bool locked)
{
	struct memreg *mr = NULL;

	debug_BUG_ON(bvma->mrs.memregs[mrid] == NULL);

	if (!locked)
		spin_lock(&bvma->mrs.memregs_lock);
	if (bvma->mrs.memregs[mrid]) {
		mr = bvma->mrs.memregs[mrid];
		bvma->mrs.memregs[mrid] = NULL;
		bvma->mrs.memregs_len--;
		if (mrid == bvma->mrs.memregs_last - 1)
			__reduce_memregs_last(bvma);
	}
	if (!locked)
		spin_unlock(&bvma->mrs.memregs_lock);

	return mr;
}

int check_creation_mrs(struct emp_mm *);
int donor_mgmt_init(struct emp_mm *bvma);
void donor_mgmt_exit(struct emp_mm *bvma);
#endif
