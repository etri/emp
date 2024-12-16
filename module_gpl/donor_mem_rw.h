#ifndef __DONOR_MEM_RW_H__
#define __DONOR_MEM_RW_H__

#include "vm.h"

void donor_mem_rw_init(struct emp_mm *);
void emp_wait_for_writeback(struct emp_mm *, struct vcpu_var *, int);
int __clear_writeback_block(struct emp_mm *, struct work_request *,
			    struct vcpu_var *, bool, bool);
void clear_in_flight_fetching_block(struct emp_vmr *vmr,
				struct vcpu_var *cpu, struct emp_gpa *head);

#endif /* __DONOR_MEM_RW_H__ */
