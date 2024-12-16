#ifdef CONFIG_EMP_RDMA
#ifndef __RDMA_H__
#define __RDMA_H__
#include <rdma/rdma_cm.h>
#include <linux/clocksource.h>
#include <linux/timex.h>

#include "config.h"
#include "vm.h"

#define MODULE_NAME "emp_rdma"

struct message {
	enum {
		MSG_MR,
		MSG_DONE
	} type;

	u64 addr;
	u32 rkey;
	u32 size;
	u32 dpid;
	u32 max_conns;
};

struct work_connection_struct {
	struct work_struct work;
	struct context *context;
};

int rdma_open(void *);
void rdma_release(void *);
int rdma_init(void);
void rdma_exit(void);

#define RDMA_CONN_READY(c)		((c) == CONTEXT_READY)
#define RDMA_CONN_COMPLETE(c)		((c) >= CONTEXT_READY)
#define RDMA_CONN_ESTABLISHING(c)	((c) >= CONTEXT_SEND_COMPLETE)

#endif
#endif /* CONFIG_EMP_RDMA */
