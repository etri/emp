#include <linux/mm.h>
#include "config.h"
#include "gpa.h"
#include "vm.h"
#include "local_page.h"
#include "block-flag.h"

bool emp_lp_lookup_pmd(struct local_page *lp, int vmr_id,
		       struct mapped_pmd **p, struct mapped_pmd **pp)
{
	bool found;

	if (EMP_LP_PMDS_EMPTY(&lp->pmds))
		return false;

	found = false;
	*p = &lp->pmds;
	*pp = *p;
	do {
		if ((*p)->vmr_id != vmr_id) {
			*pp = *p;
			*p = (*p)->next;
			continue;
		}

		found = true;
		break;
	} while (*p != &lp->pmds);

	return found;
}
EXPORT_SYMBOL(emp_lp_lookup_pmd);

bool emp_lp_insert_pmd(struct emp_mm *emm, struct local_page *lp, int vmr_id,
		       pmd_t *pmd) 
{
	struct mapped_pmd *p = &lp->pmds, *pp, *n;

	if (EMP_LP_PMDS_EMPTY(p)) {
		p->vmr_id = vmr_id;
		p->pmd = pmd;
		p->next = p;
		lp->num_pmds++;

		return true;
	}

	if (emp_lp_lookup_pmd(lp, vmr_id, &p, &pp)) {
		debug_BUG_ON(p->pmd != pmd);
		return true;
	}

	n = emp_lp_alloc_pmd(emm);
	if (n == NULL)
		return false;

	n->vmr_id = vmr_id;
	n->pmd = pmd;

	n->next = pp->next;
	pp->next = n;

	lp->num_pmds++;

	return true;
}

pmd_t *emp_lp_pop_pmd(struct emp_mm *emm, struct local_page *lp, int vmr_id)
{
	struct mapped_pmd *p, *pp;
	pmd_t *ret;

	if (!emp_lp_lookup_pmd(lp, vmr_id, &p, &pp))
		return NULL;

	ret = p->pmd;
	if (p != &lp->pmds) {
		// hit on any member of list excluding the head 
		pp->next = p->next;
		emp_lp_free_pmd(emm, p);
		/*printk("%s hit on a member of rest nodes", __func__);*/
	} else if (p->next != p) {
		// hit on the first member of multi-members list
		struct mapped_pmd *np = p->next;
		*p = *np;
		// release np
		emp_lp_free_pmd(emm, np);
		/*printk("%s hit on the first member of multi-members list", __func__);*/
	} else {
		// hit on the first member of single member list
		INIT_MAPPED_PMD(&lp->pmds);
	}

	lp->num_pmds--;

	return ret;
}

static inline void emp_init_local_page(struct local_page *lp, int vmr_id,
				       size_t gpa_index, struct page *page)
{
#if defined(CONFIG_EMP_DEBUG_LRU_LIST) || defined(CONFIG_EMP_DEBUG_PAGE_REF)
	int i;
#endif
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	for (i = 0; i < DEBUG_LRU_SIZE; i++) {
		lp->debug_lru_history[i].file = NULL;
		lp->debug_lru_history[i].line = -1;
		lp->debug_lru_history[i].type = DEBUG_LRU_UNKNOWN;
		lp->debug_lru_history[i].data = 0UL;
	}
	lp->debug_lru_next = 0;
#endif
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	for (i = 0; i < DEBUG_PAGE_REF_SIZE; i++) {
		lp->debug_page_ref_history[i].file = NULL;
		lp->debug_page_ref_history[i].line = 0;
		lp->debug_page_ref_history[i].add = 0;
		lp->debug_page_ref_history[i].sum = 0;
		lp->debug_page_ref_history[i].curr = 0;
		lp->debug_page_ref_history[i].map = 0;
		lp->debug_page_ref_history[i].vmr_id = 0;
		lp->debug_page_ref_history[i].num_pmds = 0;
		lp->debug_page_ref_history[i].timestamp = 0;
		lp->debug_page_ref_history[i].in_mmu_noti = 0;
		lp->debug_page_ref_history[i].in_io = 0;
		lp->debug_page_ref_history[i].in_will_pte = 0;
		lp->debug_page_ref_history[i].in_unmap = 0;
		lp->debug_page_ref_history[i].in_dup = 0;
		lp->debug_page_ref_history[i].in_calibrate = 0;
	}
	lp->debug_page_ref_page_len = 0;
	lp->debug_page_ref_next = 0;
	lp->debug_page_ref_sum = 0;
	lp->debug_page_ref_in_mmu_noti = 0;
	lp->debug_page_ref_in_io = 0;
	lp->debug_page_ref_in_will_pte = 0;
	lp->debug_page_ref_in_unmap = 0;
	lp->debug_page_ref_in_dup = 0;
	lp->debug_page_ref_in_calibrate = 0;
	lp->debug_page_ref_wrong = 0;
#endif
	debug_update_rss_init_local_page(lp);
	INIT_LIST_HEAD(&lp->lru_list);
	lp->page = page;
	lp->sptep = NULL;
	lp->flags = 0;

	lp->gpa_index = gpa_index;
	lp->vmr_id = vmr_id;
	/* debug_lru_set_vmr_id_mark() will be called at the caller of the caller */

	lp->num_pmds = 0;
	INIT_MAPPED_PMD(&lp->pmds);

	set_local_page_cpu_only(lp, EMP_UNKNOWN_CPU_ID);
	lp->demand_offset = 0;
	lp->w = NULL;
}

/**
 * alloc_local_page - Allocate local page info
 * @param bvma bvma data structure
 * @param mr memory register
 * @param page page to allocate local page info
 * @param page_order size of the page
 * @param gpa_offset gpa offset
 * @param gpa gpa info
 *
 * @return allocated local page info
 */
static struct local_page *
alloc_local_page(struct emp_mm *bvma, int vmr_id, struct memreg *mr,
			struct page *page, int page_order, off_t gpa_offset,
			struct emp_gpa *gpa)
{
	struct local_page *local_page;

	local_page = emp_kmem_cache_alloc(bvma->ftm.local_pages_cache, GFP_KERNEL);
	if (!local_page)
		return NULL;

	emp_init_local_page(local_page, vmr_id, gpa_offset, page);
	/* The partial map will be considered after alloc_local_page(). */
	__debug_page_ref_update_page_len(local_page, bvma_subblock_size(bvma));
	/* The following line represents that the initial reference count of
	 * the allocated pages. */
	debug_page_ref_mark(vmr_id, local_page, 1);

	emp_set_pg_mlocked(page, page_order);
	page->private = (unsigned long) gpa;
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	local_page->gpa = gpa;
	spin_lock(&bvma->ftm.local_page_list_lock);
	list_add(&local_page->elem, &bvma->ftm.local_page_list);
	spin_unlock(&bvma->ftm.local_page_list_lock);
#endif

	return local_page;
}

/**
 * free_local_page - Free local page info
 * @param bvma bvma data structure
 * @param local_page local page info
 */
static void free_local_page(struct emp_mm *bvma,
			struct local_page *local_page)
{
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	spin_lock(&bvma->ftm.local_page_list_lock);
	list_del_init(&local_page->elem);
	spin_unlock(&bvma->ftm.local_page_list_lock);
#endif
	debug_update_rss_free_local_page(local_page);
	debug_free_local_page(local_page);
	emp_kmem_cache_free(bvma->ftm.local_pages_cache, local_page);
}

/**
 * local_page_init - Initialize local page cache
 * @param bvma bvma data structure
 */
int local_page_init(struct emp_mm *bvma)
{
	bvma->ftm.local_pages_cache = emp_kmem_cache_create("local_pages_cache",
			sizeof(struct local_page), 0, 0, NULL);
	if (bvma->ftm.local_pages_cache == NULL)
		return -ENOMEM;
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	INIT_LIST_HEAD(&bvma->ftm.local_page_list);
	spin_lock_init(&bvma->ftm.local_page_list_lock);
#endif

	bvma->ftm.mapped_pmd_cache = emp_kmem_cache_create("mapped_pmd_cache",
			sizeof(struct mapped_pmd), 0, 0, NULL);
	if (bvma->ftm.mapped_pmd_cache == NULL) {
		emp_kmem_cache_destroy(bvma->ftm.local_pages_cache);
		return -ENOMEM;
	}

	bvma->lops.alloc_local_page = alloc_local_page;
	bvma->lops.free_local_page = free_local_page;
	return 0;
}

/**
 * local_page_exit - Destroy local page cache
 * @param bvma bvma data structure
 */
void local_page_exit(struct emp_mm *bvma)
{
	if (bvma->ftm.local_pages_cache) {
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
		BUG_ON(!list_empty(&bvma->ftm.local_page_list));
#endif
		emp_kmem_cache_destroy(bvma->ftm.local_pages_cache);
	}
	if (bvma->ftm.mapped_pmd_cache)
		emp_kmem_cache_destroy(bvma->ftm.mapped_pmd_cache);
}
