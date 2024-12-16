#include "vm.h"
#include "mm.h"
#include "gpa.h"
#include "remote_page.h"
#include "alloc.h"
#include "reclaim.h"
#include "block-flag.h"
#include "donor_mem_rw.h"
#include "debug.h"

DECLARE_WAIT_QUEUE_HEAD(read_wq);

static inline void clear_writeback_request_on_lru(struct work_request *w) {
	/* we need to clear flag of the head of block */
	struct local_page *local_page = emp_get_block_head(w->gpa)->local_page;
	debug_assert(w == local_page->w);
	clear_local_page_on_lru(local_page);
}

/**
 * fetch_pages - Load saved page contents from donor devices into local page
 * @param bvma bvma data structure
 * @param conn connection
 * @param gpa gpa
 * @param pgoff page offset
 * @param pglen the number of pages
 * @param rp remote page descriptor
 * @param criticality priority
 * @param head_wr work request for the head of a block
 * @param ops dma operation info
 *
 * @return work request of the READ request
 *
 * Fetch the demand data from donor
 */
static struct work_request *
fetch_pages(struct emp_mm *bvma, struct connection *conn, struct emp_gpa *gpa,
		off_t pgoff, int pglen, unsigned long long rp, int criticality,
		struct work_request *head_wr, struct work_request **tail_wr,
		struct dma_ops *ops)
{
	struct work_request *demand_w;

	/* read saved page contents into lp->page according to the local_page */
	debug_page_ref_io_beg(gpa->local_page);
	emp_get_subblock(gpa, false);
	demand_w = post_read(conn, gpa, pgoff, pglen, rp, criticality,
			     head_wr, tail_wr, ops);
	if (IS_ERR_OR_NULL(demand_w)) {
		debug_page_ref_io_end(gpa->local_page);
		emp_put_subblock(gpa);
	}
	return demand_w;
}

/**
 * emp_wait_for_writeback - Wait for the writeback requests
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 * @param pressure the number of pages to wait writeback
 *
 * Waits the write operation to donor
 */
void emp_wait_for_writeback(struct emp_mm *bvma, struct vcpu_var *cpu,
			    int pressure)
{
	int i;
	int reclaimed;
	int reclaimed_subblock = 0; /* total */
	int num_subblock = pressure >> bvma_subblock_order(bvma);
	struct emp_stm_ops *ops = &bvma->sops;
	
	/* we wait @num_subblock times, but do not force the reclaim */
	num_subblock -= emp_list_len(&cpu->local_free_page_list);
	if (num_subblock <= 0)
		return;

	for (i = 0; i < num_subblock; i++) {
		reclaimed = ops->wait_writeback_async(bvma, cpu, false);
		if (reclaimed == 0)
			break;
		reclaimed_subblock += reclaimed >> bvma_subblock_order(bvma);
		if (reclaimed_subblock >= num_subblock)
			return;
	}

	for ( ; i < num_subblock; i++) {
		reclaimed = ops->wait_writeback_async_steal(bvma, cpu);
		if (reclaimed == 0)
			return;
		reclaimed_subblock += reclaimed >> bvma_subblock_order(bvma);
		if (reclaimed_subblock >= num_subblock)
			return;
	}
}

/**
 * alloc_and_fetch_pages - Allocate local pages and fetch saved page contents to local page
 * @param bvma bvma data structure
 * @param gpa gpa
 * @param page_order the size of page
 * @param free_page allocatable freed page
 * @param vcpu working vcpu ID
 * @param head_wr work request for the head of a block
 * @param criticality priority
 * @param flag the flag of a gpa
 *
 * @retval 1: Succeed to fetch
 * @retval 0: Nothing to fetch
 * @retval -1: Error
 *
 * Fetch the contents from the remote page into the local allocatable freed page
 */
static int alloc_and_fetch_pages(struct emp_vmr *vmr, struct emp_gpa *gpa,
		unsigned long gpa_idx, int page_order, int demand_offset,
		struct vcpu_var *cpu, struct work_request *head_wr,
		struct work_request **tail_wr, int criticality,
		bool no_fetch, bool is_stale, bool io_read_mask)
{
	struct memreg *mr;
	struct local_page *local_page;
	off_t dma_len;
	int avail_dma_order;
	struct page *free_page;
	struct page *page = NULL;
	struct work_request *w = NULL;
	struct emp_mm *bvma = vmr->emm;

	free_page = _alloc_pages(bvma, page_order, demand_offset, cpu);
	if (unlikely(IS_ERR_OR_NULL(free_page)))
		return PTR_ERR(free_page);

	mr = !is_gpa_remote_page_free(gpa) ?
		bvma->mrs.memregs[get_gpa_remote_page_mrid(gpa)] : NULL;
	avail_dma_order = mr ? mr->dma_order : bvma_subblock_order(bvma);

	debug_alloc_and_fetch_pages(bvma, gpa, free_page, page_order, 
				    avail_dma_order);

	dma_len = (1 << page_order);
	/* make sure that free pages are available */
	page = free_page;

	/* allocate a local page for a dma block*/
	local_page = bvma->lops.alloc_local_page(bvma, vmr->id, mr, page,
						 page_order, gpa_idx, gpa);
	if (unlikely(!local_page)) {
		_refill_global_free_page(bvma, page);
		return -ENOMEM;
	}
	debug_page_ref_update_page_len(local_page, vmr, gpa, gpa_idx);
	debug_lru_set_vmr_id_mark(local_page, vmr->id);

	debug_BUG_ON(!list_empty(&local_page->lru_list));
	gpa->local_page = local_page;
	emp_update_rss_add(vmr, __gpa_to_page_len(vmr, gpa, gpa_idx),
				DEBUG_RSS_ADD_ALLOC_FETCH,
				gpa, DEBUG_UPDATE_RSS_SUBBLOCK);

	/* fetch a page for this local page */
	if (!no_fetch) {
		/* fetch_pages fetches contiguous pages according to the size
		 * described in local_page and increases page_order temporarily
		 * to fetch multiple pages with a request */
		debug_BUG_ON(mr == NULL);
		w = fetch_pages(bvma, mr->conn, gpa,
				get_gpa_remote_page_offset(gpa), dma_len,
				get_gpa_remote_page_val(gpa), criticality,
				head_wr, tail_wr, mr->ops);
		if (IS_ERR_OR_NULL(w)) { // failed to fetch page
			bvma->lops.free_local_page(bvma, local_page);
			page->private = 0;
			emp_clear_pg_mlocked(page, page_order);
			gpa->local_page = NULL;
			emp_update_rss_sub(vmr, __gpa_to_page_len(vmr, gpa, gpa_idx),
						DEBUG_RSS_SUB_ALLOC_FETCH_ERR,
						gpa, DEBUG_UPDATE_RSS_SUBBLOCK);
			_refill_global_free_page(bvma, page);
			return PTR_ERR(w);
		}
	} else {
		debug_BUG_ON(page_count(local_page->page) < 1);

		if (!is_stale) {
			// For the first touched pages, do zeroing.
			if (!io_read_mask)
				memset(page_address(local_page->page), 0,
					PAGE_SIZE << gpa_subblock_order(gpa));

			if (clear_gpa_flags_if_set(gpa, GPA_REMOTE_MASK))
				free_remote_page(bvma, gpa, true);
		}

		w = NULL;
		
		debug_alloc_and_fetch_pages2(vmr, gpa);
	}
	debug_check_notnull_pointer(local_page->w);
	local_page->w = w;

	return w ? 1 : 0;
}

/**
 * remove_first_wb_request - Remove the first writeback request from wb_request_list of the given cpu
 * @param bvma bvma data structure
 * @param cpu working CPU ID
 *
 * @retval NULL: Error
 * @retval n: Success
 *
 * Remove the writeback request stored in vcpu structure
 */
static inline struct work_request *
remove_first_wb_request(struct emp_mm *bvma, struct vcpu_var *cpu)
{
	struct work_request *w = NULL;
	struct list_head *wb_request_list;

	wb_request_list = &cpu->wb_request_list;

	if (VCPU_WB_REQUEST_LE_SINGLULAR(&cpu))
		goto out;

	w = list_first_entry(wb_request_list, struct work_request, sibling);
	if (WR_READY(w)) {
		list_del_init(&w->sibling);
		cpu->wb_request_size -= w->wr_size;
	} else {
		w = NULL;
	}

out:
	return w;
}

/**
 * free_work_requests - Free work requests
 * @param bvma bvma data structure
 * @param w work request
 */
static void 
free_work_requests(struct emp_mm *bvma, struct work_request *w)
{
	while (!list_empty(&w->remote_pages)) {
		struct work_request *r;

		r = list_first_entry(&w->remote_pages, struct work_request,
				remote_pages);
		list_del_init(&r->remote_pages);
		if (is_remote_page_freed(&r->remote_page))
			__free_remote_page(bvma, &r->remote_page);
		free_work_request(bvma, r);
	}

	if (is_remote_page_freed(&w->remote_page))
		__free_remote_page(bvma, &w->remote_page);
	free_work_request(bvma, w);
}

/* TODO: ZERO_BLOCK checking should be located on module_pro */
#define ZERO_BLOCK (1 << 21)
/**
 * clear_writeback_work_request - Clear completed writeback work requests
 * @param bvma bvma data structure
 * @param w work request
 * @param cpu working CPU ID
 * @param prefetch prefetch requst?
 * @param do_reclaim do reclaim?
 *
 * @retval -1: Error
 * @retval n: Success
 */
static int clear_writeback_work_request(struct emp_mm *bvma,
					struct work_request *w,
					struct vcpu_var *cpu,
					bool prefetch, bool do_reclaim)
{
	int reclaimed_pages = 0;
	struct work_request *head_wr;
	struct emp_gpa *g, *head;
	int vmr_id;

	/* wait write operations to be completed */
	wait_write(bvma, w, cpu, prefetch);

	/* if current work request is not the writeback request of head,
	 * just free this work request and return 0 as reclaimed pages.
	 */
	head_wr = w->head_wr;
	// freeing work_request is delayed until free of head_wr
	if (w->chained_ops == false && w != head_wr)
		list_add_tail(&w->remote_pages, &head_wr->remote_pages);
	if (atomic_dec_and_test(&head_wr->wr_refc) == false)
		return reclaimed_pages;

	w = head_wr;

	debug_assert(w->gpa->local_page);
	debug_assert(w->gpa->local_page->w == w);

	head = emp_get_block_head(w->gpa);
	debug_progress(head, w);
	debug_assert(____emp_gpa_is_locked(head));
	/* NOTE: CoWed block may have no vmr_id on the gpa. */
	vmr_id = head->local_page->vmr_id;

	/* remove an assumption that a head_wr is related to the head node of
	 * a block. it means that there is no restriction in order of
	 * work_requests.
	 */
	for_each_gpas_reverse(g, head) {
		debug_assert(g->local_page);

		/* TODO: ZERO_BLOCK checking should be located on module_pro */
		if ((!is_gpa_flags_set(g, ZERO_BLOCK))) {
			// counter part of get_page right
			// before post_writeback_async
			debug_page_ref_io_end(g->local_page);
			emp_put_subblock(g);
			emp_unlock_subblock(g);
		}
		if (do_reclaim)
			bvma->vops.set_gpa_remote(bvma, cpu, g);
		else
			g->local_page->w = NULL;
	}

	if (do_reclaim) {
		if (vmr_id >= 0)
			emp_update_rss_cached(bvma->vmrs[vmr_id]);
		sub_inactive_list_page_len(bvma, head);
	}

	reclaimed_pages = gpa_block_size(head);


	free_work_requests(bvma, w);

	return reclaimed_pages;
}

/**
 * __clear_writeback_block - Clear completed writeback block
 * @param bvma bvma data structure
 * @param w work request
 * @param vcpu working vcpu ID
 * @param prefetch prefetch requst?
 * @param do_reclaim do reclaim?
 *
 * @return the number of reclaimed pages
 */
int __clear_writeback_block(struct emp_mm *bvma, struct work_request *w,
			    struct vcpu_var *cpu, bool prefetch,
			    bool do_reclaim)
{
	struct work_request *s;
	int reclaimed_pages = 0, r;

	debug_progress(w, list_empty(&w->subsibling));
	while (!list_empty(&w->subsibling)) {
		s = list_first_entry(&w->subsibling, struct work_request,
				     subsibling);
		list_del_init(&s->subsibling);
		r = clear_writeback_work_request(bvma, s, cpu, prefetch,
							do_reclaim);
		reclaimed_pages += r > 0 ? r : 0;
	}
	r = clear_writeback_work_request(bvma, w, cpu, prefetch,
						do_reclaim);
	reclaimed_pages += r > 0 ? r : 0;

	return reclaimed_pages;
}

static struct work_request *
__try_pop_ready_writeback_request(struct emp_mm *bvma, struct vcpu_var *cpu)
{
	struct list_head *wb_request_list = &cpu->wb_request_list;
	struct work_request *w;

	debug_assert(spin_is_locked(&cpu->wb_request_lock));
	if (list_empty(wb_request_list))
		return NULL;

	w = list_first_entry(wb_request_list, struct work_request, sibling);
	if (!WR_READY(w))
		return NULL;

	if (emp_trylock_work_request(bvma, w)) {
		list_del_init(&w->sibling);
		clear_writeback_request_on_lru(w);
		cpu->wb_request_size -= w->wr_size;
		return w;
	} else
		return NULL;
}

/**
 * pop_writeback_request - Pop a writeback request from a list
 * @param bvma bvma data structure
 * @param cpu working CPU ID
 *
 * @retval NULL: Error
 * @retval n: Success
 */
static struct work_request *
pop_writeback_request(struct emp_mm *bvma, struct vcpu_var *cpu)
{
	struct work_request *w;

	if (list_empty(&cpu->wb_request_list))
		return NULL;

	spin_lock(&cpu->wb_request_lock);
	if (list_empty(&cpu->wb_request_list)) {
		spin_unlock(&cpu->wb_request_lock);
		return NULL;
	}

	list_for_each_entry(w, &cpu->wb_request_list, sibling) {
		if (emp_trylock_work_request(bvma, w)) {
			list_del_init(&w->sibling);
			clear_writeback_request_on_lru(w);
			cpu->wb_request_size -= w->wr_size;
			goto found;
		}
	}
	w = NULL; // if not found, initialize @w

found:
	spin_unlock(&cpu->wb_request_lock);

	return w;
}

/**
 * push_writeback_request - Add a new writeback request and remove the first one from the list
 * @param bvma bvma data structure
 * @param w work request
 * @param cpu working CPU ID
 *
 * @return working vcpu ID
 */
static int push_writeback_request(struct emp_mm *bvma, struct work_request *w,
				  struct vcpu_var *cpu)
{
	spinlock_t *s;
	int cpu_queue;
	struct work_request *removed_w = NULL;

	// the function is called after all the members of wr are linked to head_wr,
	// it means that wr_refc is equal to the number of members
	w->wr_size = w->chained_ops == false? 
		atomic_read(&w->wr_refc): w->entangled_wr_len;

	s = &cpu->wb_request_lock;

	spin_lock(s);
	cpu_queue = cpu->id;
	if (cpu->wb_request_size + w->wr_size > PER_VCPU_WB_REQUESTS_MAX(bvma))
		removed_w = __try_pop_ready_writeback_request(bvma, cpu);
	list_add_tail(&w->sibling, &cpu->wb_request_list);
	cpu->wb_request_size += w->wr_size;
	spin_unlock(s);

	debug_BUG_ON(w && w->head_wr == NULL);

	if (removed_w) {
		/* NOTE: __clear_writeback_block() frees @w. Backup the block
		 *       head that need to be unlocked. */
		struct emp_gpa *head = emp_get_block_head(removed_w->gpa);
		debug_progress(removed_w, list_empty(&removed_w->subsibling));
		__clear_writeback_block(bvma, removed_w, cpu, false, true);
		emp_unlock_block(head);
	}

	return cpu_queue;
}

/**
 * post_writeback_async - Writeback local pages to donor device
 * @param bvma bvma data structure
 * @param gpa gpa
 * @param vcpu working vcpu ID
 * @param head_wr work request for the head of a block
 * @param tail_wr work request for the tail of a block
 * @param pglen the number of pages
 * @param update_gpa need to update the gpa?
 *
 * @return work request of the writeback
 *
 * Returned struct work_request is processed by the caller function
 */
static struct work_request *
post_writeback_async(struct emp_mm *bvma, struct emp_gpa *gpa,
		     struct vcpu_var *cpu, struct work_request *head_wr,
		     struct work_request *tail_wr, int pglen, bool update_gpa)
{
	int mrid;
	struct memreg *mr;
	struct work_request *w;

	/* remote_page should not be NULL */
	debug_assert(is_gpa_remote_page_free(gpa) == false);
	debug_generate_tag(gpa);

	mrid = get_gpa_remote_page_mrid(gpa);
	gpa->last_mr_id = mrid;
	mr = bvma->mrs.memregs[mrid];

	/* write local page contents to donor device */
	do {
		int err = 0;
		w = post_write(mr->conn, gpa, get_gpa_remote_page_offset(gpa),
			       pglen, head_wr, tail_wr,
			       get_gpa_remote_page_val(gpa), &err, mr->ops);
		if (unlikely(w == NULL)) {
			if (err == -ENOMEM)
				bvma->sops.wait_writeback_async(bvma, cpu, false);
			else /* err == -ENXIO */
				return NULL;
		}
	} while (w == NULL);

	if (update_gpa) {
		/* update gpa fields and w fields */
		gpa->r_state = GPA_WB;

		debug_check_notnull_pointer(gpa->local_page->w);

		gpa->local_page->w = w;
		barrier();
	}

	INIT_LIST_HEAD(&w->sibling);
	return w;
}

/**
 * clear_writeback_block - Lookup the head of writeback work request
 * and clear completed writeback block
 * @param bvma bvma data structure
 * @param head head of the block
 * @param w work request of head of the block
 * @param cpu working vcpu ID
 * @param on_list is work_request on wb_request_list?
 * @param head_locked is head locked?
 */
static void clear_writeback_block(struct emp_mm *bvma, struct emp_gpa *head,
				  struct work_request *w, struct vcpu_var *cpu,
				  bool on_list, bool do_reclaim)
{
	struct vcpu_var *wb_vcpu;

	debug_BUG_ON(!w);
	debug_assert(____emp_gpa_is_locked(head));

	if (on_list) {
		s16 cpu_id = get_local_page_cpu(head->local_page);
		wb_vcpu = emp_get_vcpu_from_id(bvma, cpu_id);
		spin_lock(&wb_vcpu->wb_request_lock);
		debug_progress(w, cpu_id);
		debug_BUG_ON(list_empty(&w->sibling));
		list_del_init(&w->sibling);
		clear_writeback_request_on_lru(w);
		wb_vcpu->wb_request_size -= w->wr_size;
		spin_unlock(&wb_vcpu->wb_request_lock);
	}

	debug_progress(w, list_empty(&w->subsibling));
	__clear_writeback_block(bvma, w, cpu, false, do_reclaim);
}

/**
 * wait_writeback_async - Pop a writeback request and clear it
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 * @param prefetch prefetch request?
 *
 * @return the number of completed writeback
 */
static int 
wait_writeback_async(struct emp_mm *bvma, struct vcpu_var *cpu, bool prefetch)
{
	int ret = 0;
	struct work_request *w = pop_writeback_request(bvma, cpu);
	if (w) {
		/* NOTE: __clear_writeback_block() frees @w. Backup the block
		 *       head that need to be unlocked. */
		struct emp_gpa *head = emp_get_block_head(w->gpa);
		debug_progress(w, list_empty(&w->subsibling));
		ret = __clear_writeback_block(bvma, w, cpu, prefetch, true);
		emp_unlock_block(head);
	}
	return ret;
}

/**
 * wait_writeback_async_steal - Pop a writeback request of other vcpu and clear it
 * @param bvma bvma data structure
 * @param cpu waiting cpu
 *
 * @return the number of completed writeback
 */
static int wait_writeback_async_steal(struct emp_mm *emm, struct vcpu_var *waiting_cpu)
{
	int c;
	struct vcpu_var *v;
	struct work_request *w = NULL;

	for_all_vcpus_from(v, c, waiting_cpu, emm) {
		w = pop_writeback_request(emm, v);
		if (w) break;
	}

	if (!w)
		return 0;
	else {
		struct emp_gpa *head = emp_get_block_head(w->gpa);
		int ret = __clear_writeback_block(emm, w, waiting_cpu,
								false, true);
		emp_unlock_block(head);
		return ret;
	}
}

/**
 * clear_fetching_work_request - Clear completed fetch work request
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 * @param w work request
 */
static void clear_fetching_work_request(struct emp_mm *bvma,
					struct vcpu_var *cpu,
					struct work_request *w)
{
	struct work_request *head_wr;
	struct emp_gpa *gpa;

	wait_read(bvma, w, cpu, false);
	debug_check_tag(w);

	head_wr = w->head_wr;
	if (w != head_wr) {
		if (!w->chained_ops || w != head_wr->eh_wr) {
			gpa = w->gpa;
			debug_assert(gpa);
				free_remote_page(bvma, gpa, true);
			free_work_request(bvma, w);
		}
	}
	//atomic_dec_and_test returns true when the argument is zero.
	if (atomic_dec_and_test(&head_wr->wr_refc) == false)
		return;

	if (head_wr->eh_wr) {
		gpa = head_wr->eh_wr->gpa;
		debug_assert(gpa);
			free_remote_page(bvma, gpa, true);
		free_work_request(bvma, head_wr->eh_wr);
	}
	/* At this point, &head_wr->wr_refc is zero.
	 * In other words, all pages in a block have been fetched successfully. */
	gpa = head_wr->gpa;
	debug_assert(gpa);
		free_remote_page(bvma, gpa, true);
	free_work_request(bvma, head_wr);
}

/**
 * try_clear_fetching_work_request - Clear completed fetch work request if
 * 				     the work request is completed
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 * @param w work request
 *
 * @retval true: Succeed to check the completion and clear it
 * @retval false: Failed to check the completion
 */
static bool try_clear_fetching_work_request(struct emp_mm *bvma,
					    struct vcpu_var *cpu,
					    struct work_request *w)
{
	struct work_request *head_wr;

	head_wr = w->head_wr;

	/* Here, only the demand subblock is entered */
	debug_BUG_ON(w != head_wr);

	if (try_wait_read(bvma, w, cpu, false) == 0)
		return false;
	debug_check_tag(w);

	if (atomic_dec_and_test(&head_wr->wr_refc) == true) {
		struct emp_gpa *gpa = head_wr->gpa;
		debug_assert(gpa);
			free_remote_page(bvma, gpa, true);
		free_work_request(bvma, head_wr);
	}

	return true;
}


void clear_in_flight_fetching_block(struct emp_vmr *vmr,
				struct vcpu_var *cpu, struct emp_gpa *head)
{
	struct emp_mm *emm = vmr->emm;
	struct emp_gpa *g;
	struct page *page;
	struct local_page *lp;
	struct work_request *w;
	int cnt_wr = 0, cnt_lp = 0;
	bool head_inserted = false;

	if (head->local_page && !list_empty(&head->local_page->lru_list))
		head_inserted = true;

	for_each_gpas(g, head) {
		if ((lp = g->local_page) == NULL)
			continue;
		if ((w = lp->w) == NULL)
			continue;
		if (w->chained_ops && w->head_wr->eh_wr == NULL) {
			/* chained but not posted work request */
			w->chained_ops = false;
			w->state = STATE_PR_FAILED;
		}
		clear_fetching_work_request(emm, cpu, lp->w);
		cnt_wr++;
		if (head_inserted)
			continue;
		page = lp->page;
		emm->lops.free_local_page(emm, lp);
		page->private = 0;
		emp_clear_pg_mlocked(page, g->sb_order);
		g->local_page = NULL;
		_refill_global_free_page(emm, page);
		cnt_lp++;
	}

	printk(KERN_NOTICE "%s clears %d work requests and %d local pages\n",
				__func__, cnt_wr, cnt_lp);
}

/**
 * wait_read_async - Clear completed fetch work request and clear remote page
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 * @param gpa gpa of fetched page
 *
 * @retval true: clear completed fetch work request
 * @retval false: no work request to clear
 */
static bool 
wait_read_async(struct emp_mm *bvma, struct vcpu_var *cpu, struct emp_gpa *gpa)
{
	struct work_request *w = gpa->local_page->w;
	if (w == NULL)
		return false;

	clear_fetching_work_request(bvma, cpu, w);
	gpa->local_page->w = NULL;
	return true;
}

/**
 * try_wait_read_async - Clear completed fetch work request and clear remote page
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 * @param gpa gpa of fetched page
 *
 * @retval true: clear completed fetch work request
 * @retval false: no work request to clear
 */
static bool try_wait_read_async(struct emp_mm *bvma, struct vcpu_var *cpu,
				struct emp_gpa *gpa)
{
	struct work_request *w = gpa->local_page->w;
	if (w == NULL)
		return false;

	if (try_clear_fetching_work_request(bvma, cpu, w)) {
		gpa->local_page->w = NULL;
		return true;
	} else
		return false;
}


/**
 * donor_mem_rw_init - Initialize memory interface to donor
 * @param bvma bvma data structure
 *
 * Register memory interfaces
 */
void donor_mem_rw_init(struct emp_mm *bvma)
{
	bvma->sops.alloc_and_fetch_pages = alloc_and_fetch_pages;
	bvma->sops.post_writeback_async = post_writeback_async;
	bvma->sops.clear_writeback_block = clear_writeback_block;
	bvma->sops.push_writeback_request = push_writeback_request;
	bvma->sops.wait_writeback_async = wait_writeback_async;
	bvma->sops.wait_writeback_async_steal = wait_writeback_async_steal;
	bvma->sops.wait_read_async = wait_read_async;
	bvma->sops.try_wait_read_async = try_wait_read_async;
}
