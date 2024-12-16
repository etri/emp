#ifdef CONFIG_EMP_RDMA
#include <compat.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <rdma/ib_verbs.h>
#include <asm/bitops.h>

#include "vm.h"
#include "subblock.h"
#include "udma.h"
#include "rdma.h"

struct dma_ops rdma_dma_ops;

static int setup_context(struct context *, struct rdma_cm_id *);
static int connect_client(struct rdma_cm_id *id);
static int post_recv_msg(struct context *, struct rdma_cm_id *);
static int post_send_msg(struct context *, struct rdma_cm_id *);
static void schedule_destroy_context(struct context *);
static void destroy_context(struct context *);
static void modify_qp(struct context *);

#ifdef CONFIG_EMP_DEBUG
#undef pr_debug
#define pr_debug pr_info
#endif

static const char *cma_events[] = {
	"address resolved", //RDMA_CM_EVENT_ADDR_RESOLVED
	"address error",    //RDMA_CM_EVENT_ADDR_ERROR
	"route resolved",   //RDMA_CM_EVENT_ROUTE_RESOLVED
	"route error",      //RDMA_CM_EVENT_ROUTE_ERROR
	"connect request",  //RDMA_CM_EVENT_CONNECT_REQUEST
	"connect response", //RDMA_CM_EVENT_CONNECT_RESPONSE
	"connect error",    //RDMA_CM_EVENT_CONNECT_ERROR
	"unreachable",      //RDMA_CM_EVENT_UNREACHABLE
	"rejected",         //RDMA_CM_EVENT_REJECTED
	"established",      //RDMA_CM_EVENT_ESTABLISHED
	"disconnected",     //RDMA_CM_EVENT_DISCONNECTED
	"device removal",   //RDMA_CM_EVENT_DEVICE_REMOVAL
	"multicast join",   //RDMA_CM_EVENT_MULTICAST_JOIN
	"multicast error",  //RDMA_CM_EVENT_MULTICAST_ERROR
	"address change",   //RDMA_CM_EVENT_ADDR_CHANGE
	"timewait exit",    //RDMA_CM_EVENT_TIMEWAIT_EXIT
};

static const char *get_rdma_event_msg(enum rdma_cm_event_type event)
{
	int index = event;
	return (index >= 0 && index <= RDMA_CM_EVENT_TIMEWAIT_EXIT) ?
		cma_events[index] : "unrecognized event";
}

/**
 * rdma_setup_local_page - Setup local page to communicate RDMA
 * @param lpage local page info
 * @param pglen length of pages
 * @param context connection info
 * @param type dma type
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int rdma_setup_local_page(struct local_page *lpage, int pglen,
				 struct context *context, int type)
{
	enum dma_type t = (enum dma_type) type;
	enum dma_data_direction dir;

	switch (t) {
	case DMA_READ:
		dir = DMA_FROM_DEVICE;
		break;
	case DMA_WRITE:
		dir = DMA_TO_DEVICE;
		break;
	case DMA_UNKNOWN:
	default:
		dir = DMA_BIDIRECTIONAL;
		break;
	}

	lpage->dma_addr = ib_dma_map_single(context->cm_id->device,
					    page_address(lpage->page),
					    pglen << PAGE_SHIFT, dir);

	if (!lpage->dma_addr)
		return -EFAULT;
	return 0;
}

/**
 * rdma_destroy_local_page - Unmap local page to disable RDMA communication
 * @param lpage local page info
 * @param pglen length of pages
 * @param context connection info
 * @param type dma type
 */
static void rdma_destroy_local_page(struct local_page *lpage, int pglen, 
				    struct context *context, int type)
{
	enum dma_type t = (enum dma_type) type;
	enum dma_data_direction dir;

	switch (t) {
	case DMA_READ:
		dir = DMA_FROM_DEVICE;
		break;
	case DMA_WRITE:
		dir = DMA_TO_DEVICE;
		break;
	case DMA_UNKNOWN:
	default:
		dir = DMA_BIDIRECTIONAL;
		break;
	}

	ib_dma_unmap_single(context->cm_id->device,
			    lpage->dma_addr,
			    pglen << PAGE_SHIFT, dir);

	lpage->dma_addr = 0;
}

static inline int emp_process_cq(struct context *context, int budget)
{
	if (context->poll_context == POLL_CONTEXT_SOFTIRQ)
		return 0;
	return ib_process_cq_direct(context->scq, budget);
}

static inline void drain_direct_queue(struct context *context)
{
	while (atomic_read(&context->pending) > 0) {
		emp_process_cq(context, 16);
		cond_resched();
	}
}

/**
 * emp_cm_event_handler - RDMA CM (Communication Manager) handling function
 * @param id rdma cm id
 * @param event event for handling
 *
 * @retval 0: Success
 * @retval -1: Error
 *
 * Handling CM events listed below
 * + RDMA_CM_EVENT_ADDR_RESOLVED
 * + RDMA_CM_EVENT_ROUTE_RESOLVED
 * + RDMA_CM_EVENT_ESTABLISHED
 * + RDMA_CM_EVENT_DISCONNECTED
 * + RDMA_CM_EVENT_ADDR_ERROR
 * + RDMA_CM_EVENT_ROUTE_ERROR
 * + RDMA_CM_EVENT_REJECTED
 */
static int 
emp_cm_event_handler(struct rdma_cm_id *id, struct rdma_cm_event *event)
{
	int ret = 0;
	struct context *context = (struct context *)id->context;
	struct connection *conn = context->conn;

	pr_debug("%s: conn%d-%d event: %d(%s)\n", __func__,
			(conn && conn->memreg) ? conn_get_mr_id(conn) : 0xffffffff,
			context ? context->id : 0xffffffff,
			event ? event->event : -1,
			event ? get_rdma_event_msg(event->event) : "null event");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		if (setup_context(context, id))
			goto _ceh_error;
		if (post_recv_msg(context, id))
			goto _ceh_post_receive;
		if (rdma_resolve_route(id, RDMA_TIMEOUT_MS))
			goto _ceh_post_receive;
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		if ((ret = connect_client(id)) != 0) {
			pr_info("%s: conn%d-%d failed to connect server\n",
					__func__,
					conn_get_mr_id(conn),
					context->id);
			goto _ceh_post_receive;
		}
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		atomic_inc(&context->is_connected);
		wake_up_interruptible(&context->ctrl_wq);

		if (post_send_msg(context, id))
			goto _ceh_error;
		modify_qp(context);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		pr_info("%s: conn%d-%d disconnecting\n",
				__func__,
				conn_get_mr_id(conn),
				context->id);
		schedule_destroy_context(context);
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
		pr_info("%s: conn%d-%d failed to connect (address not resolved): "
				"event status: %d\n",
				__func__,
				conn_get_mr_id(conn),
				context->id,
				event->event);
		context->ctrl_state = CONTEXT_ADDR_ERROR;
		ret = -EINVAL;
		break;
	case RDMA_CM_EVENT_REJECTED:
		pr_info("%s: conn%d-%d failed to connect (rejected): "
				"event status: %d\n",
				__func__,
				conn_get_mr_id(conn),
				context->id,
				event->status);
		context->ctrl_state = CONTEXT_REJECTED;
		ret = -EBUSY;
		break;
	default:
		pr_info("%s: conn%d-%d invalid event (event id: %d)\n",
				__func__,
				conn_get_mr_id(conn),
				context->id,
				event->event);
		ret = -EPERM;
		break;
	}

	return ret;

_ceh_post_receive:
_ceh_error:
	ret = rdma_disconnect(id);
	pr_info("connection failed to connect: event: %d\n", event->event);
	return -1;
}

/**
 * setup_qp_attr - Setup attributions of RDMA queue pair
 * @param conn connection info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int setup_qp_attr(struct context *context, struct rdma_cm_id *cm_id)
{
	int ret = 0;
	struct ib_qp_init_attr *attr = &context->qp_attr;
	struct ib_device_attr dev_attr;
	struct ib_device *dev = cm_id->device;
	struct ib_udata uhw = {.outlen = 0, .inlen = 0};

	memset(&dev_attr, 0, sizeof(dev_attr));
	if (dev->ops.query_device(cm_id->device, &dev_attr, &uhw)) {
		pr_info("%s: conn%d-%d failed to query device attribute (%s)\n",
				__func__,
				conn_get_mr_id(context->conn),
				context->id,
				cm_id->device->name);
		ret = -1;
		goto _setup_qp_attr_err;
	}

	attr->qp_context = (void *)context;
	attr->sq_sig_type = IB_SIGNAL_REQ_WR;
	attr->qp_type = IB_QPT_RC;
	attr->port_num = cm_id->port_num;

	context->nr_cq = min(dev_attr.max_qp_wr, 1 << 13);
	// following magic number obtained experimentally (connectx-5)
	attr->cap.max_send_wr = context->nr_cq;
	attr->cap.max_recv_wr = context->nr_cq;
	attr->cap.max_send_sge = min(dev_attr.max_send_sge, 1 << 2);
	attr->cap.max_recv_sge = min(dev_attr.max_recv_sge, 1 << 2);

	if (context->poll_context == POLL_CONTEXT_MEMPOLL)
		context->pending_thres = 
			(context->nr_cq - (context->nr_cq >> 2));

_setup_qp_attr_err:
	return ret;
}

/**
 * setup_qp - Setup RDMA queue pair
 * @param context context info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int setup_qp(struct context *context, struct rdma_cm_id *cm_id)
{
	int ret;
	struct connection *conn = context->conn;
	struct memreg *mr = conn->memreg;
	struct ib_cq_init_attr cq_attr = {};

	cq_attr.cqe = context->qp_attr.cap.max_send_wr;
	cq_attr.comp_vector = 0;
	context->rcq = ib_alloc_cq(cm_id->device, context,
			context->nr_cq, cq_attr.comp_vector,
			IB_POLL_SOFTIRQ);
	if (IS_ERR(context->rcq)) {
		pr_info("%s: conn%d-%d failed to alloc softirq cq\n",
				__func__, mr->id, context->id);
		goto _setup_qp_err_alloc_scq;
	}

	context->scq = ib_alloc_cq(cm_id->device, context,
			context->nr_cq, cq_attr.comp_vector,
			(context->poll_context != POLL_CONTEXT_SOFTIRQ)?
			IB_POLL_DIRECT: IB_POLL_SOFTIRQ);
	if (IS_ERR(context->scq)) {
		pr_info("%s: conn%d-%d failed to alloc direct cq\n",
				__func__, mr->id, context->id);
		goto _setup_qp_err_alloc_dcq;
	}

	context->qp_attr.recv_cq = context->rcq;
	context->qp_attr.send_cq = context->scq;

	ret = rdma_create_qp(cm_id, context->pd, &context->qp_attr);
	if (ret) {
		pr_info("%s: conn%d-%d failed to create_qp: %p\n",
				__func__, mr->id, context->id,
				ERR_PTR(ret));
		goto _setup_qp_err_rdma_create_qp;
	}

	return ret;
_setup_qp_err_rdma_create_qp:
_setup_qp_err_alloc_dcq:
_setup_qp_err_alloc_scq:
	if (context->scq) {
		ib_free_cq(context->scq);
		context->scq = NULL;
	}
	if (context->rcq) {
		ib_free_cq(context->rcq);
		context->rcq = NULL;
	}
	return -1;
}

/**
 * modify_qp - Modify RDMA queue pair
 * @param context context info
 *
 * @retval 0: Success
 * @retval n: Error
 */
static void modify_qp(struct context *context)
{
	struct memreg *mr;
	struct ib_qp_attr attr;
	int path_mtu, ret, mr_id = conn_get_mr_id(context->conn);

	mr = context->conn->memreg;
	ret = ib_query_qp(context->cm_id->qp, &attr, IB_QP_PATH_MTU,
			  &context->qp_attr);
	if (ret) {
		pr_info("%s: conn%d-%d failed to query qp\n",
				__func__, mr_id, context->id);
		return;
	}

	path_mtu = attr.path_mtu;
	pr_debug("%s: conn%d-%d current qp_attr path_mtu: %d\n",
			__func__, mr_id, context->id,
			ib_mtu_enum_to_int(path_mtu));
	if (path_mtu != IB_MTU_4096) {
		attr.path_mtu = IB_MTU_4096;
		ret = ib_modify_qp(context->cm_id->qp, &attr, IB_QP_PATH_MTU);
		if (ret)
			pr_info("%s: conn%d-%d failed to modify path_mtu: "
					"current: %d rc: %x\n",
					__func__, mr_id, context->id,
					path_mtu, ret);
	}
}

static inline int drain_direct_queue_ratelimit(struct context *c, int drain)
{
	int complete, inflight;

	complete = 0;
	while ((inflight = atomic_read(&c->pending)) && 
			((inflight + drain - c->nr_cq) > 0)) {
		complete += emp_process_cq(c, drain);
		cond_resched();
	}

	if (complete) {
		pr_debug("%s: conn%d-%d %d requests are completed\n",
				__func__, conn_get_mr_id(c->conn), c->id,
				complete);
	}
	return complete;
}

/**
 * destroy_qp - Destroy RDMA queue pair
 * @param conn connection info
 * @param cm_id connection manager id
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int destroy_qp(struct context *context, struct rdma_cm_id *cm_id)
{
	int ret = -EINVAL;
	int mrid = conn_get_mr_id(context->conn);

	if (context->scq && !IS_ERR(context->scq) &&
			atomic_read(&context->pending))
		drain_direct_queue(context);

	if (cm_id->qp && !IS_ERR(cm_id->qp)) {
		ret = ib_destroy_qp(cm_id->qp);
		if (ret)
			pr_info("%s: conn%d-%d failed to destroy qp\n",
					__func__, mrid, context->id);
	}

	if (context->scq && !IS_ERR(context->scq))
		ib_free_cq(context->scq);
	if (context->rcq && !IS_ERR(context->rcq))
		ib_free_cq(context->rcq);

	return ret;
}

/**
 * setup_msg_buffer - Setup message buffer
 * @param conn connection info
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int setup_msg_buffer(struct context *context, struct rdma_cm_id *cm_id)
{
	int ret = 0;

	context->send_msg =
	  (struct message *)emp_kmalloc(sizeof(struct message), GFP_KERNEL);
	if (!context->send_msg) {
		ret = -ENOMEM;
		goto _setup_msg_buffer_send_msg;
	}
	context->send_dma_addr = ib_dma_map_single(cm_id->device,
						   context->send_msg,
						   sizeof(struct message),
						   DMA_TO_DEVICE);
	if (!context->send_dma_addr) {
		ret = -EFAULT;
		goto _setup_msg_buffer_send_dma;
	}

	context->recv_msg =
	  (struct message *)emp_kmalloc(sizeof(struct message), GFP_KERNEL);
	if (!context->recv_msg) {
		ret = -ENOMEM;
		goto _setup_msg_buffer_recv_msg;
	}
	context->recv_dma_addr = ib_dma_map_single(cm_id->device,
						   context->recv_msg,
						   sizeof(struct message),
						   DMA_FROM_DEVICE);
	if (!context->recv_dma_addr) {
		ret = -EFAULT;
		goto _setup_msg_buffer_recv_dma;
	}

	return 0;
_setup_msg_buffer_recv_dma:
	emp_kfree((void *)context->recv_msg);
	context->recv_msg = NULL;
_setup_msg_buffer_recv_msg:
	ib_dma_unmap_single(cm_id->device, context->send_dma_addr,
				sizeof(struct message), DMA_TO_DEVICE);
_setup_msg_buffer_send_dma:
	emp_kfree((void *)context->send_msg);
	context->send_msg = NULL;
_setup_msg_buffer_send_msg:
	return ret;
}

/**
 * destroy_buffer - Destroy message buffer
 * @param context context info
 */
static void destroy_buffer(struct context *context, bool on_rdma_reject)
{

	if (!on_rdma_reject && context->recv_dma_addr) {
		ib_dma_unmap_single(context->cm_id->device,
					context->recv_dma_addr,
					sizeof(struct message),
					DMA_FROM_DEVICE);
		context->recv_dma_addr = 0;
	}

	if (context->recv_msg) {
		emp_kfree((void *)context->recv_msg);
		context->recv_msg = NULL;
	}

	if (!on_rdma_reject && context->send_dma_addr) {
		ib_dma_unmap_single(context->cm_id->device,
					context->send_dma_addr,
					sizeof(struct message),
					DMA_TO_DEVICE);
		context->send_dma_addr = 0;
	}

	if (context->send_msg) {
		emp_kfree((void *)context->send_msg);
		context->send_msg = NULL;
	}
}

#define MR_FLAGS \
	(IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE)

static inline struct ib_pd *
emp_ib_alloc_pd(struct context *context, struct rdma_cm_id *cm_id)
{
#ifdef MLNX_OFED
	return ib_alloc_pd(cm_id->device);
#else
	return ib_alloc_pd(cm_id->device, 0);
#endif
}

static inline int emp_ib_get_dma_mr(struct context *context)
{
#ifdef MLNX_OFED
	context->mr = ib_get_dma_mr(context->pd, MR_FLAGS);
#else
	context->mr = context->pd->device->ops.get_dma_mr(context->pd,MR_FLAGS);
#endif
	if (IS_ERR(context->mr)) 
		return -1;

#if !defined(MLNX_OFED)
	context->mr->device = context->pd->device;
	context->mr->pd = context->pd;
	context->mr->uobject = NULL;
	context->mr->need_inval = false;
	// setting __internal_mr let the dealloc_pd do dereg_mr
	context->pd->__internal_mr = context->mr;
#endif
	return 0;
}

/**
 * setup_context - Setup context to establish RDMA connection
 * @param conn connection info
 *
 * @retval 0: Success
 * @retval -1: Error
 */
static int setup_context(struct context *context, struct rdma_cm_id *cm_id)
{
	int ret = 0;

	context->pd = emp_ib_alloc_pd(context, cm_id);
	if (!context->pd)
		goto _setup_context_error_pd;

	if (emp_ib_get_dma_mr(context))
		goto _setup_context_error_mr;
	if (setup_qp_attr(context, cm_id))
		goto _setup_context_error_qp_attr;
	if (setup_qp(context, cm_id))
		goto _setup_context_error_qp;
	if (setup_msg_buffer(context, cm_id))
		goto _setup_context_error_buffer;

	return ret;
_setup_context_error_buffer:
	destroy_qp(context, cm_id);
_setup_context_error_qp:
_setup_context_error_qp_attr:
#ifdef MLNX_OFED
	ret = ib_dereg_mr(context->mr);
#endif
	if (!ret) context->mr = NULL;
_setup_context_error_mr:
	ib_dealloc_pd(context->pd);
_setup_context_error_pd:
	return -1;
}

/**
 * destroy_context - Destroy context to disable RDMA connection
 * @param conn connection info
 */
static void 
__destroy_context(struct context *context, bool on_rdma_reject)
{
	int ret = 0, mrid = conn_get_mr_id(context->conn);

	pr_debug("%s: conn%d-%d\n", __func__, mrid, context->id);

	if (context->poll_context == POLL_CONTEXT_MEMPOLL)
		atomic_set(&context->pending, 0);

	drain_direct_queue(context);
	wait_event_interruptible(context->wr_wq,
				atomic_read(&context->wr_len) == 0);
	destroy_qp(context, context->cm_id);

	if (context->mr && !IS_ERR(context->mr)) {
#ifdef MLNX_OFED
		ret = ib_dereg_mr(context->mr);
		if (ret)
			pr_info("%s: conn%d-%d failed to ib_dereg_mr\n",
					__func__, mrid, context->id);
#endif
		context->mr = NULL;
	}

	if (context->pd && !IS_ERR(context->pd)) {
		ib_dealloc_pd(context->pd);
		if (ret)
			pr_info("%s: conn%d-%d failed to ib_dealloc_pd\n",
					__func__, mrid, context->id);
	}

	destroy_buffer(context, on_rdma_reject);
}

/**
 * destroy_context - Destroy context to disable RDMA connection
 * @param conn connection info
 */
static void destroy_context(struct context *context)
{
	return __destroy_context(context, false);
}

/* In linux-4.18.0-358 and later, rdma_connect() acquires handler_mutex,
 * which is already acquired by cma_work_handler()
 * which is the caller of emp_cm_event_handler().
 * Note that RHEL_RELEASE_VERSION(8, 4) is linux-4.18.0-240
 */
#if (RHEL_RELEASE_CODE >= 0 && RHEL_RELEASE_CODE <= RHEL_RELEASE_VERSION(8, 4))	\
			  || (RHEL_RELEASE_CODE < 0 && LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0))
#define rdma_connect_locked(id, param_ptr) rdma_connect(id, param_ptr)
#endif

/**
 * connect_client - Connect to donor machine using RDMA
 * @param id RDMA CM ID
 *
 * @retval 0: Success
 * @retval n: Error
 */
static int connect_client(struct rdma_cm_id *id)
{
	int ret = 0;
	struct rdma_conn_param param;

	memset(&param, 0, sizeof(struct rdma_conn_param));
	param.responder_resources = 16;
	param.initiator_depth = 16;
	param.retry_count = RDMA_CONN_RETRY_COUNT;
	param.rnr_retry_count = 7;

	ret = rdma_connect_locked(id, &param);
	if (ret)
		pr_info("%s failed to connect: %d\n", __func__, ret);
	pr_debug("%s complete\n", __func__);

	return ret;
}

static inline struct work_request *get_work_request_from_wc(struct ib_wc *wc)
{
	return container_of(wc->wr_cqe, struct work_request, cqe);
}

static void
handle_rdma_read_mempoll(struct ib_cq *cq, struct ib_wc *wc)
{
	struct work_request *w = get_work_request_from_wc(wc);
	struct context *c = w->context;
	struct emp_mm *emm = c->conn->emm;

	atomic_dec(&c->pending_signaled);
	atomic_sub(w->wr_size, &c->pending);

	emm->stat.post_read_mempoll += w->wr_size;
	free_work_request(emm, w);
}

static inline void update_wr_state(struct work_request *w, enum wr_state next) {
	if (likely(next == STATE_READY)) {
		if (w->chained_ops)
			WRITE_ONCE(w->head_wr->eh_wr->state, next);
		else
			WRITE_ONCE(w->state, next);
	} else if (next == STATE_PR_FAILED) {
		/* if the next state is a failure, record it on my own state */
		WRITE_ONCE(w->state, next);
	} else if (next == STATE_PW_FAILED) {
		/* for write, we wait only head wr.
		 * if not chained_ops, head_wr == w */
		WRITE_ONCE(w->head_wr->state, next);
	} else
		BUG();
}

static void handle_rdma_read(struct ib_cq *cq, struct ib_wc *wc)
{
	enum wr_state next_state = STATE_READY;
	struct work_request *w = get_work_request_from_wc(wc);
	struct context *context = (struct context *)w->context;
	int mrid = conn_get_mr_id(context->conn);

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		if (atomic_xchg(&context->disconnected, 1) == 0)
			pr_info("%s: conn%d-%d has been disconnected\n",
					__func__, mrid, context->id);
		next_state = STATE_PR_FAILED;
	} else if (unlikely(w->state != STATE_POST_READ)) {
		pr_info("%s: conn%d-%d inconsistent workrequest state: %d\n",
				__func__, mrid, context->id, w->state);
		debug_BUG_ON(w->state != STATE_READY);
	}

	atomic_sub(w->entangled_wr_len, &context->pending);
	update_wr_state(w, next_state);
#ifdef CONFIG_EMP_STAT
	atomic_inc(&((struct emp_mm *)context->conn->emm)->stat.read_comp);
#endif
}

static void handle_rdma_write(struct ib_cq *cq, struct ib_wc *wc)
{
	enum wr_state next_state = STATE_READY;
	struct work_request *w = get_work_request_from_wc(wc);
	struct context *context = (struct context *)w->context;
	int mrid = conn_get_mr_id(context->conn);

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		if (atomic_xchg(&context->disconnected, 1) == 0)
			pr_info("%s: conn%d-%d has been disconnected\n",
					__func__, mrid, context->id);
		next_state = STATE_PW_FAILED;
	} else if (unlikely(w->state != STATE_POST_WRITE)) {
		pr_info("%s: conn%d-%d inconsistent workrequest state: %d\n",
				__func__, mrid, context->id, w->state);
		debug_BUG_ON(w->state != STATE_READY);
	}

	/* NOTE: on error, all work requests receive signal */
	if (w->wr.wr.send_flags & IB_SEND_SIGNALED)
		atomic_sub(w->head_wr->entangled_wr_len, &context->pending);
	update_wr_state(w, next_state);
#ifdef CONFIG_EMP_STAT
	atomic_inc(&((struct emp_mm *)context->conn->emm)->stat.write_comp);
#endif
}

static inline struct context *get_context_from_send_wc(struct ib_wc *wc)
{
	return container_of(wc->wr_cqe, struct context, send_msg_cqe);
}

static void handle_send_msg(struct ib_cq *cq, struct ib_wc *wc)
{
	struct context *c = get_context_from_send_wc(wc);
	struct connection *conn = c->conn;

	atomic_dec(&c->pending);
	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_info("%s: conn%d-%d connection failed\n",
				__func__, conn_get_mr_id(conn), c->id);
		return;
	}

	c->ctrl_state++;
	wake_up_interruptible(&c->ctrl_wq);
}

static inline struct context *get_context_from_recv_wc(struct ib_wc *wc)
{
	return container_of(wc->wr_cqe, struct context, recv_msg_cqe);
}

/**
 * handle_recv_msg - Handle completion message for establishing connection
 * @param cq completion queue
 * @param wc work completion
 *
 * Two-sided RDMA request (RPC recv request)
 */
static void handle_recv_msg(struct ib_cq *cq, struct ib_wc *wc)
{
	struct context *c = get_context_from_recv_wc(wc);

	atomic_dec(&c->pending);
	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_info("%s: conn%d-%d failed to connect: "
				"invalid message received. status: %d\n",
				__func__, conn_get_mr_id(c->conn), c->id,
				wc->status);
		goto out;
	}

	if (atomic_read(&c->is_connected) == 0) {
		pr_info("%s: conn%d-%d not connected\n",
				__func__, conn_get_mr_id(c->conn), c->id);
		goto out;
	}

	if (c->rdma_size == c->recv_msg->size) {
		struct connection *conn = c->conn;

		c->rdma_addr = c->recv_msg->addr;
		c->rdma_rkey = c->recv_msg->rkey;

		c->ctrl_state++;

		conn->dpid = c->recv_msg->dpid;
		conn->max_conns = c->recv_msg->max_conns;
	} else {
		c->ctrl_state = CONTEXT_INVL_SIZE;
	}

	pr_debug("%s current ctrl_state: %d\n", __func__, c->ctrl_state);
	wake_up_interruptible(&c->ctrl_wq);
out:
	/* on error, context may be already destroying state */
	if (atomic_dec_and_test(&c->wr_len) &&
			(c->ctrl_state == CONTEXT_DESTROYING))
		wake_up_interruptible(&c->wr_wq);
}

/**
 * post_recv_msg - Post RECV message
 * @param conn connection info
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * Two-sided RDMA request (RPC recv request)
 */
static int post_recv_msg(struct context *context, struct rdma_cm_id *cm_id)
{
	int ret;
	struct memreg *mr = context->conn->memreg;
	struct ib_sge *sge = &context->recv_msg_sge;
	struct ib_cqe *cqe = &context->recv_msg_cqe;
	struct ib_recv_wr *wr = &context->recv_msg_wr;

	atomic_inc(&context->wr_len);
	sge->addr = context->recv_dma_addr;
	sge->length = sizeof(struct message);
	sge->lkey = context->mr->lkey;
	cqe->done = handle_recv_msg;

	wr->next = NULL;
	wr->num_sge = 1;
	wr->sg_list = sge;
	wr->wr_cqe = cqe;

	ret = ib_post_recv(cm_id->qp, wr, NULL);
	if (ret) {
		pr_info("%s: conn%d-%d failed to post recv message\n",
				__func__, mr->id, context->id);
		atomic_dec(&context->wr_len);
		return ret;
	}
	atomic_inc(&context->pending);

	return ret;
}

/**
 * post_send_msg - Post SEND message
 * @param conn connection info
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * Two-sided RDMA request (RPC send request)
 */
static int post_send_msg(struct context *context, struct rdma_cm_id *cm_id)
{
	int ret, mr_id;
	struct ib_sge *sge = &context->send_msg_sge;
	struct ib_cqe *cqe = &context->send_msg_cqe;
	struct ib_send_wr *wr = &context->send_msg_wr;

	atomic_inc(&context->wr_len);
	mr_id = conn_get_mr_id(context->conn);
	context->send_msg->type = MSG_DONE;

	sge->addr = context->send_dma_addr;
	sge->length = sizeof(struct message);
	sge->lkey = context->mr->lkey;
	cqe->done = handle_send_msg;

	wr->next = NULL;
	wr->sg_list = sge;
	wr->num_sge = 1;
	wr->wr_cqe = cqe;
	wr->opcode = IB_WR_SEND;
	wr->send_flags = IB_SEND_SIGNALED;

	wait_event_interruptible(context->ctrl_wq,
			atomic_read(&context->is_connected));

	/*context->ctrl_state = CONTEXT_SEND;*/
	context->ctrl_state++;
	ret = ib_post_send(cm_id->qp, wr, NULL);
	if (ret) {
		pr_info("%s: conn%d-%d failed to post send message\n",
				__func__, mr_id, context->id);
		ret = -EINVAL;
		goto out;
	}

	atomic_inc(&context->pending);
	while (context->ctrl_state < CONTEXT_SEND_COMPLETE) {
		emp_process_cq(context, 16);
		pr_debug("%s: conn%d-%d wait for a completion of send_msg state %d\n",
				__func__, mr_id, context->id, context->ctrl_state);
		wait_event_interruptible(context->ctrl_wq,
				context->ctrl_state >= CONTEXT_SEND_COMPLETE);
	}
	pr_debug("%s: conn%d-%d done for waiting completion of send_msg state %d\n",
			__func__, mr_id, context->id, context->ctrl_state);

out:
	atomic_dec(&context->wr_len);
	return ret;
}

/**
 * destroy_context_work - Destroy work connection to disable RDMA connection
 * @param work work info
 */
static void destroy_context_work(struct work_struct *work)
{
	struct work_connection_struct *work_connection;
	struct connection *conn;
	struct context *context;
	int mrid, ctxid, n_contexts;

	work_connection = (struct work_connection_struct *)work;
	context = work_connection->context;
	conn = context->conn;
	mrid = conn_get_mr_id(conn);
	ctxid = context->id;

	pr_debug("%s: conn%d-%d called\n", __func__, mrid, ctxid);

	// prevent allocation of work request
	conn->contexts[context->id] = NULL;
	n_contexts = --conn->n_contexts;
	if (context->cm_id) {
		struct rdma_cm_id *cm_id = context->cm_id;

		synchronize_rcu();
		if (atomic_read(&context->is_connected))
			pr_info("%s: conn%d-%d FIXME: connected flag should be zero!\n",
					__func__, mrid, ctxid);
		destroy_context(context);
		rdma_destroy_id(cm_id);
	}

	emp_kfree(context);
	emp_kfree(work_connection);

	if (atomic_dec_and_test(&conn->refcount)) {
		struct emp_mm *emm;

		pr_debug("%s: conn%d-%d refcount reaches zero. n_context: %d\n",
				__func__, mrid, ctxid, n_contexts);

		emm = ((struct memreg *)conn->memreg)->bvma;
		wake_up_interruptible(&emm->mrs.mrs_ctrl_wq);
	}
}

/**
 * schedule_destroy_context - Schedule the destroy_context_work function
 * @param conn connection info
 */
static void schedule_destroy_context(struct context *context)
{
	int mrid;
	struct work_connection_struct *work;

	if (atomic_cmpxchg(&context->is_connected, 1, 0) != 1)
		return; // destroy_context_work() is already scheduled
	context->ctrl_state = CONTEXT_DESTROYING;

	mrid = conn_get_mr_id(context->conn);
	pr_debug("%s: conn%d-%d called\n", __func__, mrid, context->id);

	work = emp_kmalloc(sizeof(struct work_connection_struct), GFP_KERNEL);
	work->context = context;

	INIT_WORK((struct work_struct *)work, destroy_context_work);
	schedule_work((struct work_struct *)work);

	pr_debug("%s: conn%d-%d exit\n", __func__, mrid, context->id);
}

static inline void __clear_rdma_conn_param(struct rdma_conn_param *p)
{
	memset(p, 0, sizeof(struct rdma_conn_param));
}

static int 
rdma_create_context(struct connection *conn, struct donor_info *donor, 
		    int poll_context)
{
	int context_id, ret;
	struct context *context;
	struct rdma_cm_id *cm_id;

	context = emp_kzalloc(sizeof(struct context), GFP_KERNEL);
	if (!context) {
		ret = -ENOMEM;
		goto _create_context_error;
	}

	for (context_id = 0; context_id < N_RDMA_QUEUES; context_id++) {
		if (conn->contexts[context_id])
			continue;
		conn->contexts[context_id] = context;
		conn->n_contexts++;
		break;
	}

	init_waitqueue_head(&context->ctrl_wq);
	context->ctrl_state = CONTEXT_CONNECTING;
	atomic_set(&context->wr_len, 0);
	init_waitqueue_head(&context->wr_wq);
	context->rdma_size = donor->size;
	context->conn = conn;
	context->id = context_id;
	atomic_set(&context->disconnected, 0);

	context->poll_context = poll_context;
	if (context->poll_context != POLL_CONTEXT_MEMPOLL)
		context->chained_ops = true;

	cm_id = rdma_create_id(&init_net, emp_cm_event_handler, context,
			       RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cm_id)) {
		pr_info("connection failed: rdma_create_id");
		ret = -EINVAL;
		goto _create_rdma_cmid_error;
	}
	context->cm_id = cm_id;

	ret = rdma_resolve_addr(cm_id, (struct sockaddr *)&(conn->local),
			(struct sockaddr *)&(conn->remote),
			RDMA_TIMEOUT_MS);
	if (ret) {
		pr_info("connection failed: rdma_resolve_addr");
		ret = -EINVAL;
		goto _create_rdma_resolve_addr_error;
	}

	return context_id;
_create_rdma_resolve_addr_error:
	rdma_disconnect(cm_id);
_create_rdma_cmid_error:
	emp_kfree(context);
_create_context_error:
	return ret;
}

/**
 * rdma_create_conn - Create connection to communicate RDMA
 * @param emm emp_mm struct
 * @param connection connection info
 * @param donor donor info
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * Establish RDMA communication between donor to EMP
 */
static int rdma_create_conn(struct emp_mm *emm, struct connection **c,
						struct donor_info *donor)
{
	int i, ret;
	bool mempoll_pref = bvma_mem_poll(emm);
	struct rdma_conn_param param;
	struct connection *conn = NULL;
	const int contexts_size = sizeof(struct context *) * N_RDMA_QUEUES;
	int poll_contexts[N_RDMA_QUEUES];

	__clear_rdma_conn_param(&param);
	param.responder_resources = 1;
	param.initiator_depth = 1;
	param.retry_count = RDMA_CONN_RETRY_COUNT;

	conn = emp_kzalloc(sizeof(struct connection), GFP_KERNEL);
	if (!conn) {
		ret = -ENOMEM;
		goto _create_conn_error;
	}

	conn->ip = donor->addr;
	conn->port = donor->port;
	conn->local.sin_family = AF_INET;
	conn->local.sin_addr.s_addr = htonl(INADDR_ANY);
	conn->local.sin_port = htons(0);

	conn->remote.sin_family = AF_INET;
	conn->remote.sin_addr.s_addr = donor->addr;
	conn->remote.sin_port = 
		htons(donor->port==0?DONOR_PORT:donor->port);
	conn->write_criticality = N_RDMA_QUEUES - 1;

	init_waitqueue_head(&conn->ctrl_wq);

	conn->contexts = emp_kzalloc(contexts_size, GFP_KERNEL);
	if (!conn->contexts) {
		ret = -ENOMEM;
		goto _create_context_error;
	}


	if (mempoll_pref) {
		poll_contexts[0] = POLL_CONTEXT_MEMPOLL;
		poll_contexts[1] = POLL_CONTEXT_MEMPOLL;
	} else {
		poll_contexts[0] = POLL_CONTEXT_DIRECT;
		poll_contexts[1] = POLL_CONTEXT_SOFTIRQ;
	}

	for (i = 2; i < N_RDMA_QUEUES; i++)
		poll_contexts[i] = POLL_CONTEXT_SOFTIRQ;

	for (i = 0; i < N_RDMA_QUEUES; i++) {
		rdma_create_context(conn, donor, poll_contexts[i]);
		atomic_inc(&conn->refcount);
	}

	*c = conn;

	pr_info("connecting to donor%x:%d\n", donor->addr, donor->port);
	return 0;
_create_context_error:
	emp_kfree(conn);
_create_conn_error:
	return ret;
}

static int __rdma_wait_for_context(struct context *context)
{
	bool rep_print;
	int ctrl_state, wait_count;
	int mr_id = conn_get_mr_id(context->conn);
	const int timeout = 90;

	wait_count = 0;
	pr_debug("wait for send_complete conn%d-%d ctrl_state: %d\n",
			mr_id, context->id, context->ctrl_state);
	do {
		wait_event_timeout(context->ctrl_wq,
				RDMA_CONN_ESTABLISHING(context->ctrl_state),
				HZ / 64);

		ctrl_state = context->ctrl_state;
		if (ctrl_state == CONTEXT_SEND)
			drain_direct_queue(context);

		switch (ctrl_state) {
		case CONTEXT_SEND:
		case CONTEXT_CONNECTING:
			wait_count++;
			if (wait_count > (64 * timeout)) {
				pr_info("conn%d-%d donor not responding: %d\n",
						mr_id, context->id, ctrl_state);
			}
			break;
		default:
			break;
		}
		if ((wait_count % 64) == 1)
			pr_info("conn%d-%d connecting... %d seconds\n", 
					mr_id, context->id, (wait_count >> 6));
	} while (!(RDMA_CONN_ESTABLISHING(ctrl_state)));
	pr_debug("completion of waiting for send_complete conn%d-%d ctrl_state: %d\n",
			mr_id, context->id, context->ctrl_state);

	rep_print = false;
	while (!RDMA_CONN_COMPLETE(ctrl_state)) {
		if (rep_print == false) {
			pr_debug("wait for conn%d-%d emp_process_cq\n",
					mr_id, context->id);
			rep_print = true;
		}
		emp_process_cq(context, 16);
		wait_event_interruptible(context->ctrl_wq,
				RDMA_CONN_COMPLETE(context->ctrl_state));
		ctrl_state = context->ctrl_state;
	}

	if (ctrl_state == CONTEXT_READY)
		drain_direct_queue(context);
	else { /* error */
		struct connection *conn = context->conn;
		struct emp_mm *emm = ((struct memreg *)conn->memreg)->bvma;

		switch (ctrl_state) {
		case CONTEXT_INVL_SIZE:
			pr_info("conn%d-%d different capacity: %lld MiB requested "
					"but donor provides %d MiB\n", mr_id,
					context->id,
					context->rdma_size,
					context->recv_msg->size);
			rdma_disconnect(context->cm_id);
			break;
		case CONTEXT_ADDR_ERROR:
			pr_info("conn%d-%d has an address error. ip: %x port: %d\n",
					mr_id, context->id, context->conn->ip,
					context->conn->port);

			conn->n_contexts--;
			conn->contexts[context->id] = NULL;
			emp_kfree(context);

			atomic_dec(&conn->refcount);
			wake_up_interruptible(&emm->mrs.mrs_ctrl_wq);

			break;
		case CONTEXT_REJECTED:
			pr_info("conn%d-%d donor not responding ip: %x port: %d\n",
					mr_id, context->id, context->conn->ip,
					context->conn->port);

			conn->n_contexts--;
			conn->contexts[context->id] = NULL;
			if (context->cm_id) {
				synchronize_rcu();
				__destroy_context(context, true);
			}
			emp_kfree(context);

			atomic_dec(&conn->refcount);
			wake_up_interruptible(&emm->mrs.mrs_ctrl_wq);

			break;
		}

		return -EINVAL;
	}

	pr_debug("done for waiting conn%d-%d state %d ctrl_state %d\n",
			mr_id, context->id, context->ctrl_state, ctrl_state);
	if (context->ctrl_state == CONTEXT_READY)
		pr_info("conn%d-%d established - donor%x:%d, "
				"(pid:%d, max_conn:%d)\n", mr_id, context->id,
				context->conn->ip, context->conn->port,
				context->conn->dpid, context->conn->max_conns);
	return 0;
}

static int rdma_wait_for_conn(struct connection *c)
{
	int i, r, ret = 0;

	for (i = 0; i < N_RDMA_QUEUES; i++) {
		if (c->contexts[i] == NULL)
			continue;
		r = __rdma_wait_for_context(c->contexts[i]);
		if (r != 0 && ret == 0)
			ret = r;
	}

	return ret;
}
/**
 * rdma_destroy_conn - Destroy connection
 * @param connection connection info
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * Destroy RDMA communication between donor to EMP
 */
static int rdma_destroy_conn(struct emp_mm *emm, struct connection *conn)
{
	int ret = 0, i, mrid;
	struct context *context;
	struct memreg *mr = (struct memreg *)conn->memreg;

	mrid = mr->id;
	pr_debug("%s: conn%d called refc: %d\n", __func__, mrid,
			atomic_read(&conn->refcount));
	
	for (i = 0; i < N_RDMA_QUEUES; i++) {
		context = conn->contexts[i];
		if (context == NULL)
			continue;

		pr_debug("%s: conn%d-%d is_connected: %d "
				"ctrl_state: %d \n",
				__func__, mrid, context->id,
				atomic_read(&context->is_connected),
				context->ctrl_state);

		if (atomic_read(&context->is_connected) == 0)
			// destroy_context_work() is already scheduled
			continue;

		// trigger disconnection of each connection
		ret = rdma_disconnect(context->cm_id);
		pr_debug("%s: conn%d-%d call rdma_disconnect() ret: %d\n",
				__func__, mrid, i, ret);
	}

	// wait for disconnection of all the related contexts
	do {
		ret = wait_event_interruptible(emm->mrs.mrs_ctrl_wq,
				atomic_read(&conn->refcount) == 0);
	} while (ret == -ERESTARTSYS);

	emp_kfree(conn->contexts);
	emp_kfree(conn);

	mr->conn = NULL;

	pr_info("%s: conn%d disconnected\n", __func__, mrid);
	return ret;
}

/**
 * rdma_post_read - Request READ to RDMA connection
 * @param conn connection info
 * @param w work request for READ operation
 * @param lpage local page structure
 * @param pgoff_rba page offset
 * @param pgoff_len size of data
 * @param criticality priority?
 * @param head_wr work request for head
 *
 * @return work request for the READ operation
 *
 * One-sided RDMA request
 */
/// pgoff in 4k granularity
static struct work_request *
rdma_post_read(struct connection *conn, struct work_request *w,
	       struct local_page *lpage, unsigned long pgoff_rba,
	       unsigned int pgoff_len, int criticality,
	       struct work_request *head_wr, struct work_request **tail_wr)
{
	int ret, inflight;
	unsigned long remote_size = pgoff_len << PAGE_SHIFT;
	unsigned long remote_base = pgoff_rba << PAGE_SHIFT;
	struct context *context = CONN_GET_CONTEXT(conn, criticality);

	atomic_set(&w->wr_refc, 0);
	INIT_LIST_HEAD(&w->sibling);

	w->sge.lkey = context->mr->lkey;
	w->sge.addr = lpage->dma_addr;
	w->sge.length = remote_size;

	w->wr.wr.opcode = IB_WR_RDMA_READ;
	w->wr.rkey = context->rdma_rkey;
	w->wr.remote_addr = context->rdma_addr + remote_base;
	w->state = STATE_POST_READ;

	w->context = context;
	w->processor_id = emp_smp_processor_id();
#ifdef LATENCY_EVAL
	w->post_time = get_ts_in_ns();
#endif
	w->head_wr =  head_wr == NULL ? w : head_wr;
	atomic_inc(&w->head_wr->wr_refc);

	if (tail_wr && context->chained_ops) {
		if (*tail_wr)
			(*tail_wr)->wr.wr.next = &w->wr.wr;
		w->entangled_wr_len = 0;
		w->wr.wr.next = NULL;
		w->wr.wr.send_flags = 0;
		w->cqe.done = handle_rdma_read;
		w->chained_ops = true;
		return w;
	}

	w->entangled_wr_len = 1;
	w->eh_wr = NULL;
	w->chained_ops = false;
	if (context->poll_context != POLL_CONTEXT_MEMPOLL) {
		w->wr.wr.send_flags = IB_SEND_SIGNALED;
		w->wr.wr.wr_cqe = &w->cqe;
		w->cqe.done = handle_rdma_read;
	} else if ((inflight = atomic_read(&context->pending)) > 
			context->pending_thres) {
		struct memreg *mr = conn->memreg;
		struct work_request *new_w;

		if (atomic_cmpxchg(&context->pending_signaled, 0, 1) == 0) {
			new_w = alloc_work_request(mr, w->dma_ops);
			if (new_w) {
				new_w->context = context;
				new_w->wr_size = inflight;
				new_w->cqe.done = handle_rdma_read_mempoll;
				new_w->remote_page = w->remote_page;
				new_w->type = TYPE_FETCHING_MEMPOLL;
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
				new_w->local_page = w->local_page;
#endif

				w->wr.wr.send_flags = IB_SEND_SIGNALED;
				w->wr.wr.wr_cqe = &new_w->cqe;
			} else {
				atomic_dec(&context->pending_signaled);
			}
		}
	}

	do {
		ret = ib_post_send(context->cm_id->qp, &w->wr.wr, NULL);
		switch (ret) {
		case 0:
	// on success
			atomic_inc(&context->pending);
			break;
		case -ENOMEM:
	// Send Queue is full or not enough resources to complete this operation
			emp_process_cq(context, 16);
			break;
		case -EINVAL:
	// Invalid value provided in wr
		case -EFAULT:
	// Invalid value provided in qp
		default:
	// On failure and no change will be done to the QP and bad_wr points to 
	// the SR that failed to be posted
			pr_info("%s: conn%d-%d failed to post rdma_read. ret: %d\n",
					__func__, conn_get_mr_id(conn), context->id,
					ret);
			BUG();
		}
	} while (ret != 0);
	return w;
}

static inline bool is_wr_read_ready(struct work_request *w)
{
	struct context *context;
	
	if (WR_READY(w))
		return true;
	if (w->chained_ops && WR_READY(w->head_wr->eh_wr))
		return true;

	context = w->context;
	if (context->poll_context == POLL_CONTEXT_MEMPOLL) {
		void *marker;

		marker = page_address(w->gpa->local_page->page);
		marker += gpa_subblock_page_size(w->gpa);
		marker -= sizeof(u64);

		if (*(volatile u64 *)marker != EMPTY_PAGE) {
			w->state = STATE_READY;
			return true;
		}
	}

	return false;
}

/**
 * rdma_wait_read - Wait for the completion of a READ request
 * @param bvma bvma data structure
 * @param w work request for the READ operation
 * @param vcpu working vcpu ID
 * @param prefetch is the request for prefetch?
 */
static int rdma_wait_read(struct emp_mm *bvma, struct work_request *w, 
			  struct vcpu_var *cpu, bool prefetch)
{
	struct context *context = w->context;
#ifdef LATENCY_EVAL
	u64 completion = 0, wait_start;
	wait_start = get_ts_in_ns();
#endif

	while (!is_wr_read_ready(w)) {
		if (WR_PR_FAILED(w))
			return -1;

		emp_process_cq(context, 16);
		cond_resched();
	}

#ifdef LATENCY_EVAL
	completion = get_ts_in_ns();
	if (completion && w->processor_id == emp_smp_processor_id())
		pr_debug("%s: conn%d-%d read_latency: %lld "
				"wait_for_completion: %lld "
				"emp_cost: %lld\n",
				__func__,
				conn_get_mr_id(context->conn),
				context->id,
				completion - w->post_time,
				completion - wait_start,
				wait_start - w->post_time);
#endif
	return 0;
}

/**
 * rdma_try_wait_read - Check the completion of a READ request
 * @param bvma bvma data structure
 * @param w work request for the READ operation
 * @param vcpu working vcpu ID
 * @param prefetch is the request for prefetch?
 *
 * @retval -1: Failed due to erroneous connection
 * @retval 0: Failed to check the completion
 * @retval 1: Succeed to check the completion
 */
static int rdma_try_wait_read(struct emp_mm *bvma, struct work_request *w,
			      struct vcpu_var *cpu, bool prefetch)
{
	struct context *context = w->context;
#ifdef LATENCY_EVAL
	u64 completion = 0, wait_start;
	wait_start = get_ts_in_ns();
#endif

	if (!is_wr_read_ready(w)) {
		if (WR_PR_FAILED(w))
			return -1;

		emp_process_cq(context, 16);
		if (!is_wr_read_ready(w))
			return 0;
	}

#ifdef LATENCY_EVAL
	completion = get_ts_in_ns();
	if (completion && w->processor_id == emp_smp_processor_id())
		pr_debug("%s: conn%d-%d read_latency: %lld "
				"wait_for_completion: %lld "
				"emp_cost: %lld\n",
				__func__,
				conn_get_mr_id(context->conn),
				context->id,
				completion - w->post_time,
				completion - wait_start,
				wait_start - w->post_time);
#endif
	return 1;
}

/**
 * rdma_post_write - Request WRITE to RDMA connection
 * @param conn connection info
 * @param w work request for READ operation
 * @param lpage local page structure
 * @param pgoff page offset
 * @param pglen size of data
 * @param head_wr work request for head
 * @param err error descriptor
 *
 * @return work request for the WRITE operation
 *
 * One-sided RDMA request
 */
static struct work_request *
rdma_post_write(struct connection *conn, struct work_request *w,
		struct local_page *lpage, unsigned long pgoff, int pglen, 
		struct work_request *head_wr, struct work_request *tail_wr,
		int *err)
{
	int ret;
	unsigned long remote_size = pglen << PAGE_SHIFT;
	unsigned long remote_base = pgoff << PAGE_SHIFT;
	struct context *context = CONN_GET_CONTEXT(conn, conn->write_criticality);

	drain_direct_queue_ratelimit(context, 1024);

	atomic_set(&w->wr_refc, 0);
	w->sge.addr = lpage->dma_addr;
	w->sge.lkey = context->mr->lkey;
	w->sge.length = remote_size;

	w->wr.wr.opcode = IB_WR_RDMA_WRITE;
	w->wr.wr.send_flags = IB_SEND_SIGNALED;
	w->wr.rkey = context->rdma_rkey;
	w->wr.remote_addr = context->rdma_addr + remote_base;
	w->cqe.done = handle_rdma_write;
	w->state = STATE_POST_WRITE;
	w->head_wr = head_wr == NULL ? w : head_wr;
	if (!head_wr || context->chained_ops == false)
		atomic_inc(&w->head_wr->wr_refc);

	w->context = context;
	w->processor_id = emp_smp_processor_id();
#ifdef LATENCY_EVAL
	w->post_time = get_ts_in_ns();
#endif
	if (context->chained_ops) {
		if (tail_wr)
			tail_wr->wr.wr.next = &w->wr.wr;
		w->entangled_wr_len = 0;
		w->wr.wr.next = NULL;
		w->wr.wr.send_flags = 0;
		w->chained_ops = true;
		return w;
	}

	w->entangled_wr_len = 1;
	w->chained_ops = false;
	w->wr.wr.send_flags = IB_SEND_SIGNALED;
	do {
		ret = ib_post_send(context->cm_id->qp, &w->wr.wr, NULL);
		switch (ret) {
		case 0:
	// on success
			atomic_inc(&context->pending);
			break;
		case -ENOMEM:
	// Send Queue is full or not enough resources to complete this operation
			emp_process_cq(context, 16);
			free_work_request((struct emp_mm *)conn->emm, w);
			w = NULL;
			*err = -ENOMEM;
			break;
		case -EINVAL:
	// Invalid value provided in wr
		case -EFAULT:
	// Invalid value provided in qp
		default:
	// On failure and no change will be done to the QP and bad_wr points to 
	// the SR that failed to be posted
			pr_info("%s: conn%d-%d failed to rdma write. ret: %d\n",
					__func__,
					conn_get_mr_id(context->conn),
					context->id,
					ret);
			BUG();
		}
	} while (ret != 0 && ret != -ENOMEM);
	return w;
}

static void mark_end_of_entangled_work_request(struct work_request *tail_wr,
					       struct work_request *eh_wr,
					       struct work_request *head_wr,
					       int eh_len)
{
	// disconnect link between previous and current w
	tail_wr->wr.wr.next = NULL;
	// set send_flags to generate CQE
	tail_wr->wr.wr.send_flags = IB_SEND_SIGNALED;
	// set head of entangled work_request that will be used by CQE handler
	tail_wr->eh_wr = eh_wr;
	// set ENTANGLED_WR_LEN at tail_wr for read request
	if (eh_wr->state == STATE_POST_READ)
		tail_wr->entangled_wr_len = eh_len;
	// set ENTANGLED_WR_LEN at head_wr for write request
	else
		head_wr->entangled_wr_len = eh_len;
	// set entangled work request to head_wr
	// 1. eh_wr != NULL indicates that the chained work requests are posted
	// 2. eh_wr->state is the shared state for the chained work requests.
	head_wr->eh_wr = eh_wr;
}

static bool rdma_post_work_req(struct work_request *eh_wr,
			       struct work_request *tail_wr,
			       struct work_request *head_wr,
			       int eh_len)
{
	int ret;
	const struct ib_send_wr *bad_wr;
	struct context *context = eh_wr->context;

	mark_end_of_entangled_work_request(tail_wr, eh_wr, head_wr, eh_len);

	do {
		ret = ib_post_send(context->cm_id->qp, &eh_wr->wr.wr, &bad_wr);
		switch (ret) {
		case 0:
	// on success
			/*pr_debug("%s ib_post_send success\n", __func__);*/
			atomic_add(eh_len, &context->pending);
			break;
		case -ENOMEM:
	// Send Queue is full or not enough resources to complete this operation
			pr_info("%s: conn%d-%d failed ret: %d wr %llx\n",
					__func__, conn_get_mr_id(context->conn),
					context->id, ret, (u64)eh_wr);
			emp_process_cq(context, 16);
			break;
		case -EINVAL:
	// Invalid value provided in wr
		case -EFAULT:
	// Invalid value provided in qp
		default:
	// On failure and no change will be done to the QP and bad_wr points to 
	// the SR that failed to be posted
			pr_info("%s: conn%d-%d failed to rdma write. ret: %d",
					__func__, conn_get_mr_id(context->conn),
					context->id, ret);
			break;
		}
	} while (ret != 0);

	return (ret == 0);
}

/**
 * rdma_wait_write - Wait for the completion of a WRITE request
 * @param bvma bvma data structure
 * @param w work request for the WRITE operation
 * @param vcpu working vcpu ID
 * @param prefetch is the request for prefetch?
 */
static int rdma_wait_write(struct emp_mm *bvma, struct work_request *w,
			   struct vcpu_var *cpu, bool prefetch)
{
	struct context *context = w->context;
#ifdef LATENCY_EVAL
	u64 completion = 0, wait_start;
	unsigned int processor_id = emp_smp_processor_id();

	wait_start = get_ts_in_ns();
#endif
	while (!WR_READY(w)) {
		if (WR_PW_FAILED(w))
			return -1;

		emp_process_cq(context, 16);
		cond_resched();
	}

#ifdef LATENCY_EVAL
	completion = get_ts_in_ns();
	if (completion && processor_id == emp_smp_processor_id())
		pr_debug("%s: conn%d-%d write_latency: %lld "
				"wait_for_completion: %lld\n",
				__func__,
				conn_get_mr_id(context->conn),
				context->id,
				completion - w->post_time,
				completion - wait_start);
#endif
	return 0;
}

static inline void __clear_work_request(struct work_request *w)
{
	memset(w, 0, sizeof(struct work_request));
}

/**
 * test_read_latency - Test the READ latency through RDMA communication
 * @param c connection info
 *
 * @return the cycle of read latency
 *
 * To check the overhead of RDMA communication, it checks the READ latency
 */
static int COMPILER_DEBUG test_read_latency(struct connection *c)
{
	u64 dma_addr;
	struct work_request w;
	struct ib_wc wc;
	struct ib_sge sge;
	struct ib_cq *cq;
	struct page *page;
	int ret, n_loop;
	u64 a, b, sum;
	u16 invalid_count;
	struct context *context = CONN_GET_CONTEXT(c, 0);

	if (!c || !RDMA_CONN_READY(c->contexts[0]->ctrl_state)) {
		pr_info("%s: conn%d-%d not ready\n", __func__,
				conn_get_mr_id(c), context->id);
		return -EINVAL;
	}
	if ((cq = context->scq) == NULL) {
		pr_info("%s: conn%d-%d not ready for cq %llx %llx %llx\n",
				__func__, conn_get_mr_id(c), context->id,
				(u64)cq, (u64)context->qp_attr.send_cq,
				(u64)context->scq);
		return -EINVAL;
	}

	page = emp_alloc_pages(GFP_HIGHUSER_MOVABLE, 0);
	if (!page || PageHWPoison(page)) {
		if (page) emp_free_pages(page, 0);
		pr_info("%s: conn%d-%d insufficient memory\n", __func__,
				conn_get_mr_id(c), context->id);
		return -ENOMEM;
	}

	dma_addr = ib_dma_map_single(context->cm_id->device,
			page_address(page), PAGE_SIZE,
			DMA_FROM_DEVICE);
	if (!dma_addr) {
		emp_free_pages(page, 0);
		return -ENOMEM;
	}

	sge.addr = dma_addr;
	sge.lkey = context->mr->lkey;
	sge.length = PAGE_SIZE;

	__clear_work_request(&w);
	w.wr.wr.num_sge = 1;
	w.wr.wr.sg_list = &sge;
	w.wr.wr.opcode = IB_WR_RDMA_READ;
	w.wr.wr.send_flags = IB_SEND_SIGNALED;
	w.wr.remote_addr = context->rdma_addr;
	w.wr.rkey = context->rdma_rkey;

	invalid_count = 0;
	for (n_loop = 0, sum = 0; n_loop < 1000; n_loop++) {
		a = get_ts_in_ns();
		ret = ib_post_send(context->cm_id->qp, &w.wr.wr, NULL);
		if (ret) {
			pr_info("%s: conn%d-%d ip: %x failed to ib_post_send\n",
					__func__, conn_get_mr_id(c), context->id,
					c->ip);
			sum = -1ULL;
			goto out;
		}

		do {
			ret = ib_poll_cq(cq, 1, &wc);
		} while (ret != 1);
		b = get_ts_in_ns();

		if (wc.status != IB_WC_SUCCESS) {
			pr_info("%s: conn%d-%d ip: %x wc->status: %d iter: %d\n",
					__func__, conn_get_mr_id(c), context->id,
					c->ip, wc.status, n_loop);
			goto out;
		}

		switch (wc.opcode) {
		case IB_WC_RDMA_READ:
			break;
		case IB_WC_SEND:
			if (wc.wr_cqe)
				wc.wr_cqe->done(cq, &wc);
			invalid_count++;
			continue;
		default:
			pr_info("%s: conn%d-%d ip: %x invalid work completion opcode: %d\n",
					__func__, conn_get_mr_id(c), context->id,
					c->ip, wc.opcode);
			goto out;
		}
		sum += b - a;
	}

	if (invalid_count < 1000) {
		sum = sum / (1000 - invalid_count);
		pr_info("%s: conn%d-%d read latency %lld us iter: %d\n",
				__func__, conn_get_mr_id(c), context->id,
				sum, 1000-invalid_count);
	} else {
		pr_info("%s: conn%d-%d read latency invalid count: %d\n",
				__func__, conn_get_mr_id(c), context->id,
				invalid_count);
	}
out:
	ib_dma_unmap_single(context->cm_id->device, dma_addr, 4096,
			DMA_BIDIRECTIONAL);
	emp_free_pages(page, 0);

	return sum;
}

static struct ib_client comm_client = {
	.name =   "emp_rdma_client",
};

/**
 * rdma_wr_ctor - Constructor of RDMA work request
 * @param opaque work request
 */
static void rdma_wr_ctor(void *opaque)
{
	struct work_request *w = (struct work_request *)opaque;
	init_waitqueue_head(&(w->wq));
	w->state = STATE_INIT;
	w->gpa = NULL;
	INIT_LIST_HEAD(&w->sibling);
	INIT_LIST_HEAD(&w->subsibling);
	INIT_LIST_HEAD(&w->remote_pages);

	/*w->wr.wr.next = NULL;*/
	w->wr.wr.sg_list = &w->sge;
	w->wr.wr.num_sge = 1;
	/*w->wr.wr.send_flags = IB_SEND_SIGNALED;*/
	/*w->wr.wr.wr_cqe = &w->cqe;*/
	w->dma_ops = &rdma_dma_ops;
}

/**
 * rdma_init_wr - Initialize a RDMA work request
 * @param w work request
 */
static void rdma_init_wr(struct work_request *w)
{
	w->wr.wr.next = NULL;
	/*w->wr.wr.sg_list = &w->sge;*/
	/*w->wr.wr.num_sge = 1;*/
	w->wr.wr.send_flags = 0;
	w->wr.wr.wr_cqe = &w->cqe;
	w->wr_size = 1;
}

int rdma_open(void *opaque)
{
	return 0;
}

void rdma_release(void *opaque)
{
}

/**
 * rdma_init - DMA(RDMA) operation registering
 *
 * @retval 0: Success
 * @retval n: Error
 *
 * Communicate data to Remote Memory using RDMA network connection
 */
int rdma_init(void)
{
	int ret;
	ret = ib_register_client(&comm_client);
	if (ret) {
		pr_info("%s ib_register_client failed\n", __func__);
		return -EINVAL;
	}
	rdma_dma_ops.create_conn = rdma_create_conn;
	rdma_dma_ops.wait_for_conn = rdma_wait_for_conn;
	rdma_dma_ops.destroy_conn = rdma_destroy_conn;
	rdma_dma_ops.wait_read = rdma_wait_read;
	rdma_dma_ops.try_wait_read = rdma_try_wait_read;
	rdma_dma_ops.wait_write = rdma_wait_write;
	rdma_dma_ops.post_read = rdma_post_read;
	rdma_dma_ops.post_write = rdma_post_write;
	rdma_dma_ops.post_work_req = rdma_post_work_req;

	rdma_dma_ops.setup_local_page = rdma_setup_local_page;
	rdma_dma_ops.destroy_local_page = rdma_destroy_local_page;
	rdma_dma_ops.wr_ctor = rdma_wr_ctor;
	rdma_dma_ops.init_wr = rdma_init_wr;

	rdma_dma_ops.test_conn = test_read_latency;

	return ret;
}

void rdma_exit(void)
{
	ib_unregister_client(&comm_client);
}
#endif /* CONFIG_EMP_RDMA */
