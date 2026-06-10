#ifndef __VCPU_VAR_H__
#define __VCPU_VAR_H__

#include <rdma/rdma_cm.h>
#include "config.h"
#include "gpa.h"

#define SLA_REMOTE (0)
#define SLA_LOCAL  (1)
#define SLA_SELTRS (2)

#define FOR_EACH_SELECTORS(i) \
	for (i = 0; i < SLA_SELTRS; i++)

#define PCPU_ID(e, c) ((e)->possible_cpus + (c))
#define VCPU_ID(e, c) ((c) - (e)->possible_cpus)

#ifdef CONFIG_EMP_VM
#define emp_get_vcpu_from_id(emm, id) \
	(IS_IOTHREAD_VCPU(id) ? per_cpu_ptr((emm)->pcpus, PCPU_ID(emm, id)) \
			      : &(emm)->vcpus[id])
#else
#define emp_get_vcpu_from_id(emm, id) \
	per_cpu_ptr((emm)->pcpus, PCPU_ID(emm, id))
#endif

/* TODO: cpu_id == 0 is not used and does not exist.
 *       Use 0~(vcpu_len - 1) for vCPUs and remove the following line.
 *		next_id = next_id == 0 ? next_id + 1 : next_id;"
 *       The follwoing macro may be enough.
 *	 	#define emp_get_next_cpu_id(emm, id) \
 *			(((id) + 1 < EMP_KVM_VCPU_LEN(emm)) ? ((id) + 1) \
 *					: (-(emm)->possible_cpus))
 */
#define emp_get_next_cpu_id(emm, id) ({ \
	int ____next_id = (id) + 1; \
	____next_id = (____next_id == 0) ? (____next_id + 1) : ____next_id; \
	____next_id = (____next_id >= EMP_KVM_VCPU_LEN(emm)) ? (-(emm)->possible_cpus) : ____next_id; \
	____next_id; \
})

/* for_all_vcpus_from: iterate all vcpu structures from @start_cpu
 * vcpu: struct vcpu_var *. The position pointer.
 * cpu_id: cpu_id iterator. DO NOT USE THIS IN THE LOOP.
 * start_cpu: struct  vcpu_var *. The starting cpu.
 * emm: struct emp_mm *
 */
#define for_all_vcpus_from(vcpu, cpu_id, start_cpu, emm) \
		for (vcpu = (start_cpu), cpu_id = EMP_UNKNOWN_CPU_ID; \
			cpu_id != (start_cpu)->id; \
			cpu_id = cpu_id == EMP_UNKNOWN_CPU_ID \
				? emp_get_next_cpu_id(emm, (start_cpu)->id) \
				: emp_get_next_cpu_id(emm, cpu_id), \
			vcpu = emp_get_vcpu_from_id(emm, cpu_id))

#ifdef CONFIG_EMP_DEBUG_PF_HISTORY
#define EMP_DEBUG_PF_HISTORY_LEN 32
struct emp_pf_data {
	long id;
	int vmr_id;
	unsigned long addr;
	int hva_or_gpa; // hva = 0, gpa = 1
	unsigned long gpa_offset;
	unsigned long head_offset;
	int is_write;
	int gpa_flag_beg;
	int head_flag_beg;
	int gpa_flag_end;
	int head_flag_end;
	int head_state_beg;
	int head_state_mid;
	int head_state_end;
	int cow_ret;
	int local_fault_ret;
	int remote_fault_ret;
	int install_pte_ret;
	int rss_count;
	int goto_code;
	int error_code;
	int page_fault_ret;
};

struct emp_pf_history {
	spinlock_t lock;
	struct vcpu_var *vcpu;
	int vcpu_id;
	long next_id;
	atomic64_t curr_idx;
	struct emp_pf_data data[EMP_DEBUG_PF_HISTORY_LEN];
	int last_show_vmr_id;
	unsigned long last_show_addr;
};
#endif /* CONFIG_EMP_DEBUG_PF_HISTORY */

struct vcpu_var {
	struct task_struct      *tsk;

	/* list of free pages which is maintained locally */
	struct emp_list         local_free_page_list;

#ifdef CONFIG_EMP_STAT
	/* statistics */
	struct {
	    u64 vma_fault;
		u64 local_fault;
		u64 remote_fault;
	    u64 alloc_pages_wait_count;
	} stat;
#endif

	int                     id;
	u64                     private;
#ifdef CONFIG_EMP_DEBUG_PF_HISTORY
	struct emp_pf_history *pf_history;
#endif
};

#endif /* __VCPU_VAR_H__ */
