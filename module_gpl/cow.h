#ifdef CONFIG_EMP_USER
#ifndef __COW_H__
#define __COW_H__
#include "config.h"
#include "emp_type.h"
#include "vm.h"
#include "remote_page.h"

static inline bool __is_cow_gpa(struct emp_gpa *gpa)
{
	return atomic_read(&gpa->refcnt) > 1;
}

bool __put_cow_remote_page(struct emp_mm *emm, struct emp_gpa *gpa);
/* return true if remote page of gpa still exist (not cleared by CoW) */
static inline bool put_cow_remote_page(struct emp_mm *emm, struct emp_gpa *gpa)
{
	if (is_gpa_remote_page_cow(gpa) == false)
		return true;
	return __put_cow_remote_page(emm, gpa);
}

int dup_vmdesc(struct emp_vmr *new_vmr, struct emp_vmr *prev_vmr,
			const bool new_vmdesc, const bool dup_dir);
long emp_get_mmu_notifier(struct emp_vmr *vmr);
void emp_put_mmu_notifier(struct emp_vmr *vmr);
void dup_list_add(struct emp_vmr *vmr, struct emp_vmr *parent, bool shared);
void dup_list_del(struct emp_vmr *vmr);
void cow_init(struct emp_mm *emm);
void cow_exit(struct emp_mm *emm);
#endif /* __COW_H__ */
#endif /* CONFIG_EMP_USER */
