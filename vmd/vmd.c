#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#ifndef MLNX_OFED
#include <rdma/rdma_verbs.h>
#endif
#include <signal.h>
#include <sys/mman.h>

#include "vmd.h"

#define u64 unsigned long long

int running;
int running_poll_cq;
size_t buf_size;
pid_t pid = 0;
int verbose = 0;
int max_conns = 3;
void *mem_stor = NULL;
ctx_t *s_ctx = NULL;
struct rdma_cm_id *listener = NULL;

conn_t **conns = NULL;
int n_conns = 0;
pthread_spinlock_t conns_lock;

#define THREAD_FAILURE ((void *) EXIT_FAILURE)
#define THREAD_SUCCESS ((void *) EXIT_SUCCESS)

#define print_error(...) fprintf(stderr, "ERROR: " __VA_ARGS__)

#define return_error(ret,...) do { \
	print_error(__VA_ARGS__); \
	return (ret); \
} while(0)

#define print_info(x...) if (verbose) { \
	fprintf(stdout, x); \
};

#define MB_SHIFT (20)

/**
 * sig_handler - Handling signals
 * @param sig signal ID
 *
 * Handle signals. in this case, it handles a termination signal
 * and disconnect rdma
 */
static void sig_handler(int sig) 
{
	// we ignore SIGINT for preventing unintended Ctrl-C
	if (sig == SIGINT) {
		fprintf(stderr, "SIGINT(%d) is ignored to "
				"prevent unintended termination. "
				"Use SIGTERM instead of SIGINT "
				"for terminating the program.\n", sig);
		return;
	}

	print_info("signal(%d) received\n", sig);
	running = 0;
	running_poll_cq = 0;
	if (listener && listener->verbs)
		rdma_disconnect(listener);
}

/**
 * on_completion - Check the work completion info
 * @param work completion info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int on_completion(struct ibv_wc *wc)
{
	conn_t *conn = (conn_t *)wc->wr_id;

	if (wc->status != IBV_WC_SUCCESS)
		return_error(-1, "%s: status(%d) is not IBV_WC_SUCCESS(%d).\n",
				__func__, wc->status, IBV_WC_SUCCESS);

	if (wc->opcode & IBV_WC_RECV) {
		conn->recv_state++;
		print_info("client state: %s\n", recv_state_str[conn->recv_state]);

		if (conn->ready_to_send == 0) {
			conn->ready_to_send = 1;
			print_info("set conn->ready_to_send flag\n");
		}
	} else if (wc->opcode == IBV_WC_SEND) {
		conn->send_state++;
		print_info("server state: %s\n", send_state_str[conn->send_state]);
	} else {
		printf("ERROR: %s: invalid opcode(%d) on completion queue\n", 
				__func__, wc->opcode);
	}
	
	if (conn->send_state == SEND_DONE_SENT &&
			conn->recv_state == RECV_DONE_RECV) {
		printf("donor(%d) RDMA connection established\n", pid);
	}

	return 0;
}

/**
 * poll_cq - Thread to wait for work completion
 * @param dummy not used
 *
 * Waiting a single work completion.
 */
static void *poll_cq(void *c) 
{
	struct ibv_cq *cq;
	struct ibv_wc wc;
	void *cq_ctx = NULL;

	while (s_ctx && s_ctx->ctx && running_poll_cq) {
		if (ibv_get_cq_event(s_ctx->ch, &cq, &cq_ctx)) {
			fprintf(stderr, "ibv_get_cq_event() failed\n");
			pthread_exit(THREAD_FAILURE);
		}

		ibv_ack_cq_events(cq, 1);

		if (ibv_req_notify_cq(cq, 0)) {
			fprintf(stderr, "ibv_get_cq_event() failed\n");
			pthread_exit(THREAD_FAILURE);
		}

		while (ibv_poll_cq(cq, 1, &wc)) {
			if (on_completion(&wc))
				pthread_exit(THREAD_FAILURE);
		}
	}

	pthread_exit(THREAD_SUCCESS);
}

/**
 * create_poll_thread - Create a poll_cq thread
 * @param c connection info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int create_poll_thread(ctx_t *sctx)
{
	if (sctx == NULL || sctx->ctx == NULL)
		return_error(-1, "failed to create cp_poller_thread\n");

	if (pthread_create(&s_ctx->poller, NULL, poll_cq, NULL))
		return_error(-1, "failed to create cp_poller_thread\n");

	return 0;
}

/**
 * build_qp_attr - Initialize the attribute of a queue pair
 * @param context context info
 * @param qp_attr attribute for a queue pair to initialze
 */
static void build_qp_attr(ctx_t *s_ctx, struct ibv_context *context,
			  struct ibv_qp_init_attr *qp_attr)
{
	struct ibv_device_attr dev_attr;

	memset(qp_attr, 0, sizeof(*qp_attr));

	qp_attr->qp_type = IBV_QPT_RC;
	qp_attr->sq_sig_all = 1;
	qp_attr->send_cq = s_ctx->cq;
	qp_attr->recv_cq = s_ctx->cq;

	if (ibv_query_device(context, &dev_attr)) {
		qp_attr->cap.max_send_wr = 1;
		qp_attr->cap.max_recv_wr = 1;
		qp_attr->cap.max_send_sge = 1;
		qp_attr->cap.max_recv_sge = 1;
	} else {
		// workaround for EDR connection
		qp_attr->cap.max_send_wr = MIN(dev_attr.max_qp_wr, 1 << 13);
		qp_attr->cap.max_recv_wr = MIN(dev_attr.max_qp_wr, 1 << 13);
		qp_attr->cap.max_send_sge = MIN(dev_attr.max_sge, 1 << 2);
		qp_attr->cap.max_recv_sge = MIN(dev_attr.max_sge, 1 << 2);
	}
}

/**
 * register_memory - Register memory to store RDMA messages
 * @param conn connection info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int register_memory(conn_t *conn)
{
	conn->rdma_region = mem_stor;
	conn->rdma_remote_mr = ibv_reg_mr(s_ctx->pd,
			conn->rdma_region, buf_size,
			IBV_ACCESS_LOCAL_WRITE|
			IBV_ACCESS_REMOTE_READ|
			IBV_ACCESS_REMOTE_WRITE);
	if (conn->rdma_remote_mr == NULL)
		return_error(-1, "ibv_reg_mr for rdma_remote_mr failed: "
				"%d(%s)\n", errno, strerror(errno));
	return 0;
}

static void unregister_memory(conn_t *conn)
{
	ibv_dereg_mr(conn->rdma_remote_mr);
	conn->rdma_remote_mr = NULL;
	conn->rdma_region = NULL;
}

/**
 * register_msg_memory - Register memory to store SEND/RECV messages
 * @param conn connection info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int register_msg_memory(conn_t *conn)
{
	conn->send_mr = rdma_reg_write(conn->id, conn->send_msg, sizeof(msg_t));
	if (conn->send_mr == NULL) {
		return_error(-1, "ibv_reg_mr for send_mr failed: "
				"%d(%s)\n", errno, strerror(errno));
	}

	conn->recv_mr = rdma_reg_write(conn->id, conn->recv_msg, sizeof(msg_t));
	if (conn->recv_mr == NULL) {
		ibv_dereg_mr(conn->send_mr);
		return_error(-1, "ibv_reg_mr for recv_mr failed: "
				"%d(%s)\n", errno, strerror(errno));
	}

	return 0;
}

static void unregister_msg_memory(conn_t *conn)
{
	if (conn->send_mr)
		ibv_dereg_mr(conn->send_mr);
	if (conn->recv_mr)
		ibv_dereg_mr(conn->recv_mr);
}

/**
 * post_receives - Post a RECV request
 * @param conn connection info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int post_receives(conn_t *conn)
{
	struct ibv_recv_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;

	wr.wr_id = (uintptr_t)conn;
	wr.next = NULL;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	sge.addr = (uintptr_t)conn->recv_msg;
	sge.length = sizeof(msg_t);
	sge.lkey = conn->recv_mr->lkey;

	if (ibv_post_recv(conn->qp, &wr, &bad_wr))
		return_error(-1, "ibv_post_recv() failed\n");
	conn->recv_state++;

	return 0;
}

/**
 * build_context - Create context info
 * @param dev_name device name for connection
 *
 * @retval 0: Success
 * @retval -1: Error
 *
 * Open a context by the used device
 * and build protection domain, completion event channel / completion queues
 */
static int build_context(char* dev_name)
{
	int ret = 0;
	int i;
	struct ibv_device **dev_list = ibv_get_device_list(NULL);
	struct ibv_context **ctx_list = rdma_get_devices(NULL);

	/* Get an active device to allocate protection domain.
	 * We can register memory using the protection domain in advance. */
	if (!s_ctx || !dev_list || !ctx_list) {
		ret = -1;
		print_error("failed to get ibv_devices\n");
		goto out;
	}

	for (i = 0; dev_list[i]; i++) {
		if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
			s_ctx->ctx = ctx_list[i];
			break;
		}
	}

	if (s_ctx->ctx == NULL) {
		print_error("failed to get context\n");
		ret = -1;
		goto error_show_list;
	}

	s_ctx->pd = ibv_alloc_pd(s_ctx->ctx);
	if (s_ctx->pd == NULL) {
		print_error("ibv_alloc_pd() failed\n");
		ret = -1;
		goto error_show_list;
	}

	s_ctx->ch = ibv_create_comp_channel(s_ctx->ctx);
	if (s_ctx->ch == NULL) {
		print_error("ibv_create_comp_channel() failed\n");
		ret = -1;
		goto error_show_list;
	}

	s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->ch, 0);
	if (s_ctx->cq == NULL) {
		print_error("ibv_create_cq() failed\n");
		ret = -1;
		goto error_show_list;
	}

	if (ibv_req_notify_cq(s_ctx->cq, 0)) {
		print_error("ibv_req_notify() failed\n");
		ret = -1;
		goto error_show_list;
	}

error_show_list:
	if (ret != 0) {
		for (i = 0; dev_list[i]; i++) {
			printf("IB device list[%d]: name: %s context: %p\n",
					i,
					ibv_get_device_name(dev_list[i]),
					ctx_list[i]);
		}
	}

	rdma_free_devices(ctx_list);
	ibv_free_device_list(dev_list);

out:
	return ret;
}

static void free_conn(conn_t *conn)
{
	if (conn->send_msg) free(conn->send_msg);
	if (conn->recv_msg) free(conn->recv_msg);
	free(conn);
}

static conn_t *allocate_conn(void)
{
	conn_t *conn;

	conn = (conn_t *)malloc(sizeof(conn_t));
	if (!conn) {
		fprintf(stderr, "failed to allocate memory\n");
		return NULL;
	}
	memset(conn, 0, sizeof(conn_t));

	conn->send_msg = malloc(sizeof(msg_t));
	if (!conn->send_msg) {
		fprintf(stderr, "failed to allocate memory\n");
		goto out;
	}
	memset(conn->send_msg, 0, sizeof(msg_t));

	conn->recv_msg = malloc(sizeof(msg_t));
	if (!conn->send_msg || !conn->recv_msg) {
		fprintf(stderr, "failed to allocate memory\n");
		goto out;
	}
	memset(conn->recv_msg, 0, sizeof(msg_t));

	return conn;
out:
	free_conn(conn);
	return NULL;
}

static int allocate_conn_id(conn_t *conn)
{
	int i, conn_id;

	pthread_spin_lock(&conns_lock);
	if (n_conns >= max_conns) {
		pthread_spin_unlock(&conns_lock);
		return -1;
	}

	for (i = 0; i < max_conns; i++) {
		if (conns[i])
			continue;
		conns[i] = conn;
		conn_id = i;
		break;
	}
	n_conns++;
	pthread_spin_unlock(&conns_lock);

	return conn_id;
}

static int free_conn_id(conn_t *conn)
{
	int i, conn_id = -1;

	pthread_spin_lock(&conns_lock);
	for (i = 0; i < max_conns; i++) {
		if (conns[i] != conn)
			continue;
		conn_id = i;
		conns[i] = NULL;
		n_conns--;
		break;
	}
	pthread_spin_unlock(&conns_lock);

	return conn_id;
}

/**
 * build_connection - Build queue pair and initialize connection info
 * @param id RDMM CM ID
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int build_connection(struct rdma_cm_id *id)
{
	conn_t *conn;

	struct ibv_qp_init_attr qp_attr;

	if (!s_ctx || !s_ctx->ctx || s_ctx->ctx != id->verbs)
		return_error(-1, "cannot handle events in more than one context.\n");

	conn = allocate_conn();
	if (!conn) {
		print_error("build_connection() failed. connection == NULL\n");
		goto build_connection_err;
	}

	if (allocate_conn_id(conn) < 0) {
		print_error("cannot allocate more connections.\n");
		goto build_connection_conn_err;
	}

	if (register_memory(conn)) {
		print_error("cannot register memory.\n");
		goto build_connection_register_err;
	}

	build_qp_attr(s_ctx, id->verbs, &qp_attr);

	if(rdma_create_qp(id, s_ctx->pd, &qp_attr)) {
		print_error("rdma_create_qp() failed\n");
		goto build_connection_create_qp;
	}

	id->context = conn;
	conn->id = id;
	conn->qp = id->qp;
	conn->send_state = SEND_INIT;
	conn->recv_state = RECV_INIT;
	conn->ready_to_send = 0;

	if (register_msg_memory(conn)) {
		print_error("register_msg_memory() failed\n");
		goto build_connection_register_msg_memory_err;
	}

	if (post_receives(conn)) {
		print_error("post_receives() failed\n");
		goto build_connection_post_receives_err;
	}

	print_info("%s success. id: %llx\n", __func__, (u64)id);
	return 0;

build_connection_post_receives_err:
	unregister_msg_memory(conn);
build_connection_register_msg_memory_err:
	rdma_destroy_qp(conn->id);
build_connection_create_qp:
	unregister_memory(conn);
build_connection_register_err:
	free_conn_id(conn);
build_connection_conn_err:
	free_conn(conn);
build_connection_err:
	return -1;
}

static void destroy_connection(conn_t *conn)
{
	rdma_destroy_qp(conn->id);
	unregister_msg_memory(conn);
	unregister_memory(conn);
	free_conn_id(conn);
	free_conn(conn);
}

/**
 * build_params - Build connection parameters
 * @param params connection parameters
 */
static void build_params(struct rdma_conn_param *params)
{
	memset(params, 0, sizeof(struct rdma_conn_param));
	params->initiator_depth = 16;
	params->responder_resources = 16;
	params->rnr_retry_count = 7; /* infinite retry */
}

/**
 * on_connect_request - Handling a connection request
 * @param id RDMA CM ID
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int on_connect_request(struct rdma_cm_id *id)
{
	struct rdma_conn_param cm_params;

	print_info("receiving connection request...");
	if (build_connection(id))
		return_error(-1, "ERROR: build_connection() failed\n");

	build_params(&cm_params);
	if (rdma_accept(id, &cm_params)) 
		return_error(-1, "ERROR: rdma_accept() failed\n");

	print_info("done.\n");

	return 0;
}

/**
 * send_msg - Post SEND request
 * @param conn connection info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int send_msg(conn_t *conn)
{
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;
	int count;

	memset(&wr, 0, sizeof(wr));

	wr.wr_id = (uintptr_t)conn;
	wr.opcode = IBV_WR_SEND;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;

	sge.addr = (uintptr_t)conn->send_msg;
	sge.length = sizeof(msg_t);
	sge.lkey = conn->send_mr->lkey;

	count = 0;
	while(!conn->ready_to_send) {
		print_info("waiting for RDMA connection... %d\n", ++count);
		sleep(1);
	}

	if (ibv_post_send(conn->qp, &wr, &bad_wr))
		return_error(-1, "ibv_post_send() failed\n");
	return 0;
}

/**
 * send_mr - Send memory region info to client
 * @param context connection info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int send_mr(conn_t *conn)
{
	if (conn->send_state != SEND_INIT)
		return -1;

	conn->send_msg->type = MSG_MEMORY_REGION;
	conn->send_msg->addr = (uint64_t)conn->rdma_remote_mr->addr;
	conn->send_msg->rkey = (uint32_t)conn->rdma_remote_mr->rkey;
	conn->send_msg->size = (uint32_t)(buf_size >> MB_SHIFT);
	conn->send_msg->dpid = pid;
	conn->send_msg->max_conns = max_conns;

	return send_msg(conn);
}

/**
 * on_connection - Handling connection
 * @param context connection info
 *
 * @retval 0: Success
 * @retval -1: Error
 *
 * Send memory register
 */
static int on_connection(void *context)
{
	int ret;
	conn_t *conn = (conn_t *)context;

	ret = send_mr(conn);
	conn->send_state++;

	return ret;
}

/**
 * on_disconnect - Handling disconnection
 * @param id RDMA CM ID
 *
 * Deregister memory regions and destroy CM ID
 */
static int on_disconnect(struct rdma_cm_id *id)
{
	conn_t *conn = (conn_t *)id->context;

	print_info("%s called\n", __func__);

	printf("RDMA connection closed.\n");

	destroy_connection(conn);
	rdma_destroy_id(id);

	return (n_conns == 0);
}

/**
 * on_event - Handling RDMA CM events
 * @param event event ID
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int on_event(struct rdma_cm_event *event)
{
	print_info("%s event: %d\n", __func__, event->event);
	switch(event->event) {
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		return on_connect_request(event->id);
	case RDMA_CM_EVENT_ESTABLISHED:
		return on_connection(event->id->context);
	case RDMA_CM_EVENT_DISCONNECTED:
		return on_disconnect(event->id);
	default:
		return_error(-1, "%s: unknown event: %d\n",
				__func__, event->event);
	}
	return -1;
}

static void print_help(char *cmd)
{
	fprintf(stderr, "%s <options>\n", cmd);
	fprintf(stderr, "<options>\n");
	fprintf(stderr, " -m <MEMORY SIZE IN MB>\n");
	fprintf(stderr, " -p <PORT>\n");
	fprintf(stderr, " -d <DEVICE NAME>\n");
	fprintf(stderr, " -c <#CONNECTIONS>\n");
	return;
}

int main(int argc, char **argv) {
	struct sockaddr_in addr;
	struct rdma_cm_event *event = NULL;
	struct rdma_event_channel *ec = NULL;
	uint32_t port = 19675;
	int i, opt, madv, cme, ret = EXIT_SUCCESS;
	char dev_name[DEV_SIZE];

	struct sigaction sa;

	buf_size = BUF_SIZE;
	while ((opt = getopt(argc, argv, "m:p:d:c:v")) != -1) {
		switch (opt) {
		case 'm':
			buf_size = atol(optarg);
			buf_size = ((size_t)buf_size << MB_SHIFT);

			printf("donation size: ");
			if (verbose)
				printf("0x%lx bytes ", buf_size);
			if (buf_size % (1UL << MB_SHIFT))
				printf("%.3f MiB\n", (double)buf_size /
						(double)(1UL << MB_SHIFT));
			else
				printf("%ld MiB\n", buf_size >> MB_SHIFT);
			break;
		case 'p':
			port = atoi(optarg);
			printf("custom port: %d\n", port);
			break;
		case 'd':
			memset(dev_name, 0, DEV_SIZE);
			snprintf(dev_name, DEV_SIZE, "%s", optarg);
			printf("device name: %s\n", dev_name);
			break;
		case 'c':
			max_conns = atoi(optarg);
			printf("max_conns: %d\n", max_conns);
			if (max_conns < 1) {
				print_help(argv[0]);
				return EXIT_FAILURE;
			}
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			print_help(argv[0]);
			return EXIT_FAILURE;
		}
	}

	pid = getpid();

	mem_stor = memalign(HPAGE_SIZE, buf_size);
	if (!mem_stor) {
		ret = EXIT_FAILURE;
		fprintf(stderr, "failed to allocate memory\n");
		goto out;
	}
	madv = madvise(mem_stor, buf_size, MADV_HUGEPAGE);
	if (madv)
		fprintf(stderr, "madvice rc: %d\n", madv);
	printf("memstor base: 0x%lx\n", (unsigned long)mem_stor);

	printf("allocating memstor... ");
	fflush(stdout);
	if (mlock(mem_stor, buf_size) == 0)
		printf("complete\n");
	else
		printf("lazy allocation\n");

	conns = (conn_t **)malloc(sizeof(conn_t *) * max_conns);
	if (conns == NULL) {
		ret = EXIT_FAILURE;
		fprintf(stderr, "failed to allocate memory\n");
		goto out;
	}
	memset(conns, 0, sizeof(conn_t *) * max_conns);
	n_conns = 0;
	pthread_spin_init(&conns_lock, PTHREAD_PROCESS_SHARED);

	s_ctx = (ctx_t *)malloc(sizeof(ctx_t));
	if (!s_ctx) {
		ret = EXIT_FAILURE;
		fprintf(stderr, "failed to allocate memory\n");
		goto out;
	}
	memset(s_ctx, 0, sizeof(ctx_t));

	if (build_context(dev_name) < 0) {
		ret = EXIT_FAILURE;
		goto out;
	}

	running_poll_cq = 1;
	if (create_poll_thread(s_ctx) < 0) {
		ret = EXIT_FAILURE;
		goto out;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	sa.sa_handler = sig_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGKILL, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	running = 1;

	while (running) {
		ec = rdma_create_event_channel();
		if (ec == NULL)
			return_error(EXIT_FAILURE, "rdma_create_event_channel() failed\n");

		if (rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP))
			return_error(EXIT_FAILURE, "rdma_create_id() failed\n");

		if (rdma_bind_addr(listener, (struct sockaddr *)&addr))
			return_error(EXIT_FAILURE, "rdma_bind_addr() failed\n");

		if (rdma_listen(listener, 10)) /*  backlog=10 is arbitrary */
			return_error(EXIT_FAILURE, "rdma_listen() failed\n");

		port = ntohs(rdma_get_src_port(listener));
		printf("donor(%d) listening on port %d\n", pid, port);

loop:
		while ((cme = rdma_get_cm_event(ec, &event)) == 0) {
			struct rdma_cm_event event_copy;

			memcpy(&event_copy, event, sizeof(*event));
			rdma_ack_cm_event(event);

			if (on_event(&event_copy))
				break;
		}

		// we ignore SIGINT to prevent unintended Ctrl-C
		if (running && cme == -1) {
			int errsv = errno;
			if (errsv == EINTR)
				goto loop;
		}

		rdma_destroy_id(listener);
		rdma_destroy_event_channel(ec);

		listener = NULL;
		ec = NULL;
	}
	printf("terminating donor server (pid: %d)\n", pid);
out:
	if (conns) {
		running_poll_cq = 0;
		pthread_join(s_ctx->poller, NULL);
		for (i = 0; i < max_conns; i++) {
			if (conns[i] == NULL)
				continue;
			destroy_connection(conns[i]);
		}
		free(conns);
	}
	if (mem_stor)
		free(mem_stor);
	if (s_ctx) {
		ctx_t *c = s_ctx;
		s_ctx = NULL;
		free(c);
	}

	return ret;
}
