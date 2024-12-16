#include <linux/version.h>

#include "vm.h"
#include "mm.h"
#include "alloc.h"
#include "debug.h"
#include "reclaim.h"
#include "block-flag.h"

/**
 * is_local_free_pages_list_empty - Check if the local free page list is empty
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 *
 * @retval true: Empty
 * @retval false: Non-empty
 *
 * Returns true when vcpu-local free page list is empty.
 */
static bool 
is_local_free_pages_list_empty(struct emp_mm *bvma, struct vcpu_var *cpu)
{
	// We check the length (atomic variable) since this can be called
	// without locking.
	return emp_list_len(&cpu->local_free_page_list) == 0;
}

/**
 * clear_page_state - Clear the state of a page
 * @param bvma bvma data structure
 * @param page page to clear state
 */
void clear_page_state(struct emp_mm *bvma, struct page *page)
{
#ifdef CONFIG_EMP_BLOCK
	if (bvma->config.use_compound_page == false) {
		page->flags &= ~(PAGE_FLAGS_CHECK_AT_PREP);
		return;
	}

	page->flags &= ~(PAGE_FLAGS_CHECK_AT_PREP & ~PG_head_mask);
#else
	page->flags &= ~(PAGE_FLAGS_CHECK_AT_PREP);
#endif
}

/**
 * push_local_free_page - Push a free page into thread-local free page list
 * @param bvma bvma data structure
 * @param page page
 * @param cpu working vcpu ID
 */
bool COMPILER_DEBUG push_local_free_page(struct emp_mm *bvma, struct page *page,
					 struct vcpu_var *cpu)
{
	const int free_lpages_len = bvma->ftm.per_vcpu_free_lpages_len;
	struct emp_list *local_list = &cpu->local_free_page_list;

	debug_push_local_free_page(page);

	if (unlikely(cpu == NULL))
		return false;

	/* if local free page list is full, return false.
	 * To prevent imbalance in free pages in other vcpus. */
	if (emp_list_len(local_list) >= free_lpages_len)
		return false;

	emp_list_lock(local_list);
	emp_list_add_tail(&page->lru, local_list);
	emp_list_unlock(local_list);

	return true;
}

static inline struct page *
get_next_free_page(struct emp_list *free_page_list)
{
	struct list_head *elem = emp_list_pop_head(free_page_list);
	return elem ? container_of(elem, struct page, lru) : NULL;
}

static inline struct page *
get_free_page_from_list(struct list_head *cur)
{
	return container_of(cur, struct page, lru);
}

/**
 * pop_local_free_page - Pop a free page from thread-local free page list
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 *
 * @return free page
 */

struct page * COMPILER_DEBUG 
pop_local_free_page(struct emp_mm *bvma, struct vcpu_var *cpu)
{
	struct page *p;
	struct emp_list *local_list = &cpu->local_free_page_list;

	/* returns NULL if the free local page is empty */
	if (emp_list_len(local_list) == 0)
		return NULL;

	/* get a free page from free local page list */
	emp_list_lock(local_list);
	p = get_next_free_page(local_list);
	emp_list_unlock(local_list);

	debug_pop_local_free_page(p);

	return p;
}

/**
 * __alloc_page - Allocate pages with the given order
 * @param emm emp_mm data structure
 *
 * @return allocated page
 */
static struct page *__alloc_page(struct emp_mm *emm)
{
	gfp_t gfp;
	struct page *page;
	int page_order = bvma_subblock_order(emm);

	/* setup get free page flags */
	gfp = GFP_HIGHUSER_MOVABLE; //allocated pages will be used by user
#ifdef CONFIG_EMP_BLOCK
	if (emm->config.use_compound_page) 
		gfp |= __GFP_COMP;
	else
		gfp &= ~__GFP_COMP; //compound page must not be used here
#else
	gfp &= ~__GFP_COMP; //compound page must not be used here
#endif

	/* allocate pages with the given order */
#ifdef CONFIG_EMP_PREFER_DCPMM
	// gpa_dir and gpa descriptor prefer node 2
	// and local cache prefers node 3
	page = emp_alloc_pages_node(3, gfp, page_order);
#else
	page = emp_alloc_pages(gfp, page_order);
#endif
	/* PageHWPoison: returns true for pages with problems */
	if (!page || PageHWPoison(page)) {
		printk("failed to allocated memory\n");
		if (page)
			emp_free_pages(page, page_order);
		return NULL;
	}

	/* Clear page states and increase reference counts.
	 *
	 * When pages are allocated, the first page is the only page with
	 * the increased reference count value. 
	 *
	 * This loop increases the reference counts of following pages.
	 * We have to increase reference counts of following pages because
	 * kernel will try to release the pages with reference count zero. */
	clear_page_state(emm, page);
#ifdef CONFIG_EMP_BLOCK
	/* without CONFIG_EMP_BLOCK, page_order is always 0 */
	if (page_order && emm->config.use_compound_page == false) {
		int i;
		for (i = 1; i < (1 << page_order); i++) {
			page_ref_inc(page + i);
			clear_page_state(emm, page + i);
		}
	}
#endif

	/* Acquire locks for all page descriptors in allocated pages.
	 * The locks will be released when the pages are popped from free list.
	 *
	 * <Purpose>
	 *  (1) To check whether it is in free page list or not.
	 *  (2) To detect code blocks which access the page in free page list. */
	_emp_lock_page(page, page_order);

	/* update the length of allocated pages */
	atomic_add(1 << page_order, &emm->ftm.alloc_pages_len);

	return page;
}

/**
 * get_global_free_page - Returns a free page from global free page list of emm
 * @param emm emm data structure
 *
 * @return a global free page
 */
struct page *get_global_free_page(struct emp_mm *emm)
{
	struct page *page;
	struct emp_list *free_page_list = &emm->ftm.free_page_list;

	if (emp_list_len(free_page_list) == 0)
		return NULL;

	/* get a node that contains free page from list */
	emp_list_lock(free_page_list);
	page = get_next_free_page(free_page_list);
	emp_list_unlock(free_page_list);
	return page;
}

/**
 * _refill_global_free_page - Inserts the given page to free list of emm
 * @param emm emm data structure
 * @param page page
 */
void _refill_global_free_page(struct emp_mm *emm, struct page *page)
{
	int subblock_size = bvma_subblock_size(emm);
	int subblock_order = bvma_subblock_order(emm);
	struct emp_list *free_page_list = &emm->ftm.free_page_list;

#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	debug_WARN(page_count(page) != 1,
		"reference count of page is not 1 at %s. page_count: %d\n",
		__func__, page_count(page));
#endif

	if (atomic_read(&emm->ftm.free_pages_reclaim) > 0 &&
			(emp_list_len(free_page_list) > EMP_MAIN_CPU_LEN(emm))) {
		if (atomic_add_unless(&emm->ftm.free_pages_reclaim, 
						-subblock_size, 0) == 0)
			goto no_reclaim;

		_emp_unlock_page(page, subblock_order);
		emp_free_pages(page, subblock_order);

		atomic_sub(subblock_size, &emm->ftm.alloc_pages_len);

		return;
	}

no_reclaim:
	emp_list_lock(free_page_list);
	emp_list_add_tail(&page->lru, free_page_list);
	emp_list_unlock(free_page_list);
	wake_up_interruptible(&emm->ftm.free_pages_wq);

	return;
}

/**
 * check_alloc_pages_available - Check if allocating a page is available
 * @param bvma bvma data structure
 *
 * @retval true: Available
 * @retval false: Not available
 */
bool check_alloc_pages_available(struct emp_mm *bvma)
{
	atomic_t *total_pages = &bvma->ftm.alloc_pages_len;

	if (atomic_read(total_pages) < LOCAL_CACHE_MAX(bvma))
		return true;

	return false;
}

/**
 * __alloc_page_from_host - Allocate a page from host
 * @param emm emm data structure
 *
 * @return allocated page
 */
static struct page *__alloc_page_from_host(struct emp_mm *emm)
{
	if (check_alloc_pages_available(emm) == false)
		return NULL;

	return __alloc_page(emm);
}

/**
 * __alloc_page_from_writeback_local - Allocate a page after waiting local writeback request
 * @param bvma bvma data structure
 * @param cpu working vcpu ID
 *
 * @return allocated page
 *
 * First, it waits for the writeback from a working vcpu. \n
 * After writeback, the page is freed and it can be available to allocation. \n
 * This function uses theses pages to allocate.
 */
static struct page *
__alloc_page_from_writeback_local(struct emp_mm *bvma, struct vcpu_var *cpu)
{
	if (bvma->sops.wait_writeback_async(bvma, cpu, false) == 0)
		return NULL;

	return pop_local_free_page(bvma, cpu);
}

/**
 * __alloc_page_from_writeback_global - Steal the writeback page from other cpus
 * @param bvma bvma data structure
 * @param working vcpu ID
 *
 * @return allcated page
 *
 * It waits the writeback request on the I/O thread and steals
 * the page from I/O thread
 */
static struct page *
__alloc_page_from_writeback_global(struct emp_mm *bvma, struct vcpu_var *cpu)
{
	if (bvma->sops.wait_writeback_async_steal(bvma, cpu) == 0)
		return NULL;

	return pop_local_free_page(bvma, cpu);
}

/**
 * wait_pages_available - Wait for available pages
 * @param bvma bvma data structure
 * @param vcpu working vcpu ID
 *
 * @retval 0: condition false
 * @retval n: condition true
 * @retval -n: interrupted by a signal
 */
int wait_pages_available(struct emp_mm *bvma, struct vcpu_var *cpu)
{
	int res;

	res = wait_event_interruptible_timeout(
			bvma->ftm.free_pages_wq, //wait queue
			is_local_free_pages_list_empty(bvma, cpu) || //condition to wakeup
			emp_list_len(&bvma->ftm.free_page_list) ||
			(atomic_read(&bvma->ftm.alloc_pages_len) <
			 LOCAL_CACHE_MAX(bvma)), HZ/10);

	return res;
}

/**
 * flush_local_free_pages - Delete the entries in local free lists & deallocate remote pages
 * @param bvma bvma data structure
 */
void flush_local_free_pages(struct emp_mm *bvma, struct vcpu_var *cpu)
{
	struct emp_list *global_list = &bvma->ftm.free_page_list;
	struct emp_list *local_list = &cpu->local_free_page_list;
	debug_assert(spin_is_locked(&global_list->lock));
	emp_list_lock(local_list);
	emp_list_splice_tail_emp_list(local_list, global_list);
	emp_list_unlock(local_list);
}

/**
 * alloc_exit - Delete remained entries in local free lists and free them
 * @param emm emm data structure
 */
void COMPILER_DEBUG alloc_exit(struct emp_mm *emm)
{
	struct page *page;
	int subblock_order = bvma_subblock_order(emm);
	int subblock_size = bvma_subblock_size(emm);
	struct list_head *cur, *n;
	struct emp_list *free_page_list = &emm->ftm.free_page_list;
	unsigned long num_subblock = 0;

	might_sleep();

	emp_list_lock(free_page_list);
#ifdef CONFIG_EMP_BLOCK
	if (subblock_order && emm->config.use_compound_page == false) {
		int i;
		int remained;
		emp_list_for_each_safe(cur, n, free_page_list) {
			/* To prevent CPU stuck, breathe every 4GB */
			num_subblock++;
			if ((num_subblock & 0x3ff) == 0)
				cond_resched();

			page = get_free_page_from_list(cur);
			emp_list_del_init(cur, free_page_list);
			_emp_unlock_page(page, subblock_order);
#ifdef CONFIG_EMP_BLOCK
			remained = 0;
			/* without CONFIG_EMP_BLOCK, subblock_order is always 0. */
			for (i = 1; i < subblock_size; i++) {
				page_ref_dec(page + i);
				remained += page_ref_count(page + i);
				INIT_LIST_HEAD(&(page + i)->lru);
			}

			page_ref_add(page, remained);
			debug_page_ref_mark_page(-100, page, remained);
#endif

			if (PageUnevictable(page))
				ClearPageUnevictable(page);
			emp_free_pages(page, subblock_order);
		}
	} else {
		emp_list_for_each_safe(cur, n, free_page_list) {
			/* To prevent CPU stuck, breathe every 4GB */
			num_subblock++;
			if ((num_subblock & 0x3ff) == 0)
				cond_resched();

			page = get_free_page_from_list(cur);
			emp_list_del_init(cur, free_page_list);
			_emp_unlock_page(page, subblock_order);
			if (PageUnevictable(page))
				ClearPageUnevictable(page);
			emp_free_pages(page, subblock_order);
		}
	}
#else /* !CONFIG_EMP_BLOCK */
	emp_list_for_each_safe(cur, n, free_page_list) {
		/* To prevent CPU stuck, breathe every 4GB */
		num_subblock++;
		if ((num_subblock & 0x3ff) == 0)
			cond_resched();

		page = get_free_page_from_list(cur);
		emp_list_del_init(cur, free_page_list);
		_emp_unlock_page(page, subblock_order);
		if (PageUnevictable(page))
			ClearPageUnevictable(page);
		emp_free_pages(page, subblock_order);
	}
#endif /* !CONFIG_EMP_BLOCK */
	debug_assert(emp_list_empty(free_page_list));
	atomic_sub(num_subblock * subblock_size, &emm->ftm.alloc_pages_len);
	emp_list_unlock(free_page_list);

	debug_alloc_exit(emm);
}

#ifdef CONFIG_EMP_BLOCK
static void 
mark_empty_page(struct emp_mm *emm, struct page *page, int order, int demand)
{
	void *addr, *addr_end;

	if (!bvma_mark_empty_page(emm))
		return;
	/*if (demand == -1)*/
		/*return;*/

	addr_end = page_address(page + (1 << order));
	for (addr = page_address(page) + PAGE_SIZE - sizeof(u64);
			addr < addr_end; addr += PAGE_SIZE) {
		*(volatile u64 *)(addr) = EMPTY_PAGE;
	}
}
#else
static inline void 
mark_empty_page(struct emp_mm *emm, struct page *page, int order, int demand){}
#endif

/**
 * _alloc_pages - Allocates a page with the page order for vcpu
 * @param bvma bvma data structure
 * @param page_order page order (size of the page)
 * @param vcpu working vcpu ID
 * @param demand_sb_offset demand_offset when the page is used
 * 			for demand sub-block (-1: Not demand)
 *
 * @return allocatable page's page data structure
 *
 * Searching the page to allocate in the order below
 * + (1) try to get a free page from thread-local free page list
 * + (2) get a free page from global free page list
 * + (3) If code flows to here, obviously there is not enough memory. \n
 *   Now we have to wait for a free page to be allocated.
 * + (3)-1 wait for writeback requests of local vcpu
 * + (3)-2 generate a writeback requests
 * + (3)-3 wait for writeback requests of all vcpus
 * + (3)-4 wait for global free page list to be filled.
 */
/* allocates a page with the page order for vcpu. */
struct page *_alloc_pages(struct emp_mm *bvma, int page_order, 
			  int demand_offset, struct vcpu_var *cpu)
{
	int res;
	struct page *page;
	int num_try = 0;
#ifdef CONFIG_EMP_DEBUG
	int __num_try = 0;
	int __num_try_inner = 0;
#endif

	WARN_ON(page_order != bvma_subblock_order(bvma));

	/* (1) try to get a free page from thread-local free page list */
	page = pop_local_free_page(bvma, cpu);
	if (page) _emp_unlock_page(page, page_order);
	
	while (!page) {
#ifdef CONFIG_EMP_DEBUG
		__num_try++;
#endif
		/* allocate free pages in free page list
		 * if its length is below predefined threshold. */
		if ((page = __alloc_page_from_host(bvma))) {
			_emp_unlock_page(page, page_order);
			break;
		}

		/* (2) get a free page from global free page list */
		if ((page = get_global_free_page(bvma))) {
			_emp_unlock_page(page, page_order);
			break;
		}

		/* If code flows to here, obviously there is not enough memory */ 

		/* (3) now we have to wait for a free page to be allocated. */
		do {
#ifdef CONFIG_EMP_DEBUG
			__num_try_inner++;
#endif
			/* (3)-1 wait for writeback requests of normal vcpus */
			if ((page = __alloc_page_from_writeback_local(bvma, cpu))) {
				_emp_unlock_page(page, page_order);
				break;
			}

			if (unlikely(check_alloc_pages_available(bvma)))
				break;

			/* (3)-2 generate a writeback requests.
			 * By triggering pressure hadling routines for inactive lists,
			 * try to retrieve free pages. */ 
			res = reclaim_emp_pages(bvma, cpu,
					(1 << bvma_block_order(bvma)), true);
			if (res > 0)
				break;
			else if (unlikely(res < 0)) /* error */
				return ERR_PTR(-ENXIO);

			/* (3)-3 wait for writeback requests of IO vcpus */
			if ((page = __alloc_page_from_writeback_global(bvma, cpu))) {
				_emp_unlock_page(page, page_order);
				break;
			}

			if (unlikely(check_alloc_pages_available(bvma)))
				break;

			/* Increasing the size of window for writeback requests */
			cpu->post_writeback.weight++;

			/* (3)-4 wait for global free page list to be filled.
			 * The code below wakes up this thread when there is
			 * a new insertion to global free page list */
			res = wait_pages_available(bvma, cpu);
			if (res <= 0 && ((++num_try) % 10 == 0)) {
				/* Since timeout for wait_pages_available() is HZ/10,
				 * this message is shown at most once per second.
				 */
				printk(KERN_ERR "WARN: hard to alloc pages for EMP. "
						"emm: %d cpu: %d "
						"free_pages: %d alloc_pages: %d\n",
						bvma->id, cpu->id,
						emp_list_len(&bvma->ftm.free_page_list),
						atomic_read(&bvma->ftm.alloc_pages_len));
			}
		} while (res <= 0);
	}

	mark_empty_page(bvma, page, page_order, demand_offset);

	if (!PageUnevictable(page))
		SetPageUnevictable(page);
	ClearPageReferenced(page);

	debug__alloc_pages(page, page_order);

	return page;
}
