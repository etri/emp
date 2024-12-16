#ifndef __CM_SVR_H__
#define __CM_SVR_H__

#define PAGE_SIZE  (4096)
#define HPAGE_SIZE (2UL << 20)
#define BUF_SIZE   ((1UL << 30) * 32)
#define DEV_SIZE   (256)

#define MIN(a, b) (a < b? a: b)

enum msg_type { MSG_MEMORY_REGION, MSG_DONE };

enum recv_state {
	RECV_INIT,
	RECV_CONNTECTED,
	RECV_DONE_RECV
};

static char *recv_state_str[] = {
	"recv initialized",
	"recv posted",
	"recv complete"
};

enum send_state {
	SEND_INIT,
	SEND_MR_SENT,
	//SEND_RDMA_SENT,
	SEND_DONE_SENT
};

static char *send_state_str[] = {
	"send initialized",
	"send posted - delivering mr",
	"send complete - mr delivered"
};

typedef struct {
	enum msg_type type;
	uint64_t addr;
	uint32_t rkey;
	uint32_t size;
	pid_t    dpid;
	uint32_t max_conns;
} msg_t;

typedef struct {
	struct rdma_cm_id *id;
	struct ibv_qp *qp;

	int ready_to_send;

	struct ibv_mr *recv_mr;
	struct ibv_mr *send_mr;
	struct ibv_mr *rdma_remote_mr;

	msg_t *recv_msg;
	msg_t *send_msg;

	char *rdma_region;

	enum recv_state recv_state;
	enum send_state send_state;
} conn_t;

typedef struct {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_comp_channel *ch;

	pthread_t poller;
} ctx_t;

#endif
