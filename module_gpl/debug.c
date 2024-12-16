#ifdef CONFIG_EMP_DEBUG
#include <linux/kvm_host.h>

#include "config.h"
#include "gpa.h"
#include "vm.h"
#include "debug.h"
#include "kvm_mmu.h"
#include "page_mgmt.h"
#include "block.h"
#include "block-flag.h"
#include "paging.h"
#include "udma.h"
#include "remote_page.h"
#include "lru.h"
#include "pcalloc.h"

#define IS_INVALID_GFN_OFFSET(x) ((x) == (0))

/* TODO: hardcoded debugging code for checking eager writeback block */
#define ZERO_BLOCK (1 << 23)
#define EAGER_WBR  (1 << 24)

/******************** EMP DEBUG BULK MESSAGE LOCK ***********************/

static DEFINE_SPINLOCK(__debug_bulk_msg_lock);

inline void emp_debug_bulk_msg_lock(void)
{
	spin_lock(&__debug_bulk_msg_lock);
}
EXPORT_SYMBOL(emp_debug_bulk_msg_lock);

inline void emp_debug_bulk_msg_unlock(void)
{
	spin_unlock(&__debug_bulk_msg_lock);
}
EXPORT_SYMBOL(emp_debug_bulk_msg_unlock);

/******************** EMP DEBUG RSS *************************************/
#ifdef CONFIG_EMP_DEBUG_RSS
const char *debug_rss_add_str[NUM_DEBUG_RSS_ADD_ID] = {
	"INSTALL_HPTES",
	"WRITEBACK_CURR",
	"INACTIVE_CURR",
	"WRITEBACK_CURR_PRO",
	"INACTIVE_CURR_PRO",
	"FAULT_GPA",
	"ALLOC_FETCH",
	"PUT_MAX_BLOCK",
	"COW_OTHER_ACTIVE",
	"COW_INACTIVE",
	"COW_WRITEBACK",
};

const char *debug_rss_sub_str[NUM_DEBUG_RSS_SUB_ID] = {
	"WRITEBACK_PREV",
	"INACTIVE_PREV",
	"WRITEBACK_PREV_PRO",
	"INACTIVE_PREV_PRO",
	"UNMAP_PTES",
	"UNMAP_GPAS",
	"UNMAP_MAX_BLOCK_PART",
	"UNMAP_MAX_BLOCK",
	"PUT_LOCAL_PAGE",
	"SET_REMOTE",
	"ALLOC_FETCH_ERR",
	"COW_INACTIVE",
	"COW_WRITEBACK",
};

const char *debug_rss_add_kernel_str[NUM_DEBUG_RSS_ADD_KERNEL_ID] = {
	"VMA_OPEN",
	"COW_MULTI_ACTIVE",
};

const char *debug_rss_sub_kernel_str[NUM_DEBUG_RSS_SUB_KERNEL_ID] = {
	"FREE_GPA_DIR",
	"COW_MULTI_ACTIVE",
};

void __emp_update_rss_show(struct emp_vmr *vmr, const char *func)
{
	int i;
	long rss_add_total = atomic_long_read(&vmr->debug_rss_add_total);
	long rss_sub_total = atomic_long_read(&vmr->debug_rss_sub_total);
	long rss_add_kernel_total = atomic_long_read(&vmr->debug_rss_add_kernel_total);
	long rss_sub_kernel_total = atomic_long_read(&vmr->debug_rss_sub_kernel_total);
	dprintk("DEBUG_RSS: (%s) vmr: %d pid: %d mm: %p rss_cache: %ld "
		"rss_add: %ld rss_sub: %ld "
		"rss_add_kernel: %ld rss_sub_kernel: %ld rss_sum: %ld "
		"host_rss: %ld\n",
			func,
			vmr->id,
			current->pid,
			vmr->host_mm,
			atomic_long_read(&vmr->rss_cache),
			rss_add_total, rss_sub_total,
			rss_add_kernel_total, rss_sub_kernel_total,
			rss_add_total + rss_add_kernel_total - rss_sub_total - rss_sub_kernel_total,
			get_mm_counter(vmr->host_mm, MM_FILEPAGES));
	for (i = 0; i < NUM_DEBUG_RSS_ADD_ID; i++)
		dprintk("DEBUG_RSS: (%s) vmr: %d pid: %d mm: %p rss_add[%02d]: %8ld (%s)\n",
				func,
				vmr->id,
				current->pid,
				vmr->host_mm,
				i,
				atomic_long_read(&vmr->debug_rss_add[i]),
				debug_rss_add_str[i]);
	for (i = 0; i < NUM_DEBUG_RSS_SUB_ID; i++)
		dprintk("DEBUG_RSS: (%s) vmr: %d pid: %d mm: %p rss_sub[%02d]: %8ld (%s)\n",
				func,
				vmr->id,
				current->pid,
				vmr->host_mm,
				i,
				atomic_long_read(&vmr->debug_rss_sub[i]),
				debug_rss_sub_str[i]);
	for (i = 0; i < NUM_DEBUG_RSS_ADD_KERNEL_ID; i++)
		dprintk("DEBUG_RSS: (%s) vmr: %d pid: %d mm: %p rss_add_kernel[%02d]: %8ld (%s)\n",
				func,
				vmr->id,
				current->pid,
				vmr->host_mm,
				i,
				atomic_long_read(&vmr->debug_rss_add_kernel[i]),
				debug_rss_add_kernel_str[i]);
	for (i = 0; i < NUM_DEBUG_RSS_SUB_KERNEL_ID; i++)
		dprintk("DEBUG_RSS: (%s) vmr: %d pid: %d mm: %p rss_sub_kernel[%02d]: %8ld (%s)\n",
				func,
				vmr->id,
				current->pid,
				vmr->host_mm,
				i,
				atomic_long_read(&vmr->debug_rss_sub_kernel[i]),
				debug_rss_sub_kernel_str[i]);
}

#ifdef CONFIG_EMP_DEBUG_RSS_PROGRESS
static inline void __debug_update_rss_progress(struct emp_vmr *vmr, struct local_page *lp, char *file, int line, int is_add)
{
	int next = lp->debug_rss_progress_next;
	struct debug_rss_progress *progress = &lp->debug_rss_progress[next];
	progress->file = file;
	progress->line = line;
	progress->vmr_id = vmr->id;
	progress->is_add = is_add;
	lp->debug_rss_progress_next = (next + 1) % DEBUG_RSS_PROGRESS_SIZE;
}

static inline void __debug_update_rss_show_progress(struct local_page *lp)
{
	int i = lp->debug_rss_progress_next;
	int printed = 0;
	struct debug_rss_progress *p;

	do {
		p = &lp->debug_rss_progress[i];
		if (p->file == NULL)
			goto next;
		printed = 1;
		printk(KERN_ERR "[DEBUG_RSS] (PROGRESS) lp: %016lx (%02d) vmr_id: %3d %s at: %s:%d\n",
					(unsigned long) lp, i, p->vmr_id,
					p->is_add == 1 ? "ADD" : "SUB",
					p->file, p->line);
next:
		i = (i + 1) % DEBUG_RSS_PROGRESS_SIZE;
	} while (i != lp->debug_rss_progress_next);

	if (printed == 0)
		printk(KERN_ERR "[DEBUG_RSS] (PROGRESS) lp: %016lx no progress\n",
					(unsigned long) lp);
	else
		printk(KERN_ERR "[DEBUG_RSS] (PROGRESS) lp: %016lx end of progress\n",
					(unsigned long) lp);
}
#else
#define __debug_update_rss_progress(vmr, lp, file, line, is_add) do {} while (0)
#define __debug_update_rss_show_progress(lp) do {} while (0)
#endif

/* NOTE: In gcc-11, calling WARN_ONCE() with -O0 optin causes compile errors. */
static void ____debug_update_rss_warn_once_vmr_id(const char *func, int vmr_id) {
	WARN_ONCE(true, "%s: vmr_id: %d >= CONFIG_EMP_DEBUG_RSS_MAX_VMRS(%d)\n",
				func, vmr_id, CONFIG_EMP_DEBUG_RSS_MAX_VMRS);
}

static void COMPILER_DEBUG __debug_update_rss_add(struct emp_vmr *vmr, struct local_page *lp, char *file, int line) {
	int vmr_id = vmr->id;
	int i = vmr_id / (sizeof(u64)*8);
	int j = vmr_id % (sizeof(u64)*8);
	__debug_update_rss_progress(vmr, lp, file, line, 1);
	if (unlikely(vmr_id >= CONFIG_EMP_DEBUG_RSS_MAX_VMRS)) {
		____debug_update_rss_warn_once_vmr_id(__func__, vmr_id);
		return;
	}
	if (lp->debug_rss_bitmap[i] & (1ULL << j)) {
		printk(KERN_ERR "[DEBUG_RSS] ERROR: double-ADD RSS vmr_id: %d gpa_index: %ld at %s:%d\n",
				vmr_id, lp->gpa_index, file, line);
		__debug_update_rss_show_progress(lp);
		BUG();
	} else
		lp->debug_rss_bitmap[i] |= (1ULL << j);
}

static void COMPILER_DEBUG __debug_update_rss_sub(struct emp_vmr *vmr, struct local_page *lp, char *file, int line) {
	int vmr_id = vmr->id;
	int i = vmr_id / (sizeof(u64)*8);
	int j = vmr_id % (sizeof(u64)*8);
	__debug_update_rss_progress(vmr, lp, file, line, 0);
	if (unlikely(vmr_id >= CONFIG_EMP_DEBUG_RSS_MAX_VMRS)) {
		____debug_update_rss_warn_once_vmr_id(__func__, vmr_id);
		return;
	}
	if ((lp->debug_rss_bitmap[i] & (1ULL << j)) == 0) {
		printk(KERN_ERR "[DEBUG_RSS] ERROR: double-SUB RSS vmr_id: %d gpa_index: %ld at %s:%d\n",
				vmr_id, lp->gpa_index, file, line);
		__debug_update_rss_show_progress(lp);
		BUG();
	} else
		lp->debug_rss_bitmap[i] &= ~(1ULL << j);
}

void COMPILER_DEBUG
debug_update_rss_add(struct emp_vmr *vmr, long val, int ID, int by_emp,
			struct emp_gpa *gpa, int mode, char *file, int line)
{
	debug_assert(val >= 0);
	if (by_emp) {
		debug_assert(ID < NUM_DEBUG_RSS_ADD_ID);
		atomic_long_add(val, &vmr->debug_rss_add[ID]);
		atomic_long_add(val, &vmr->debug_rss_add_total);
	} else {
		debug_assert(ID < NUM_DEBUG_RSS_ADD_KERNEL_ID);
		atomic_long_add(val, &vmr->debug_rss_add_kernel[ID]); \
		atomic_long_add(val, &vmr->debug_rss_add_kernel_total); \
	}

	if (mode == DEBUG_UPDATE_RSS_SUBBLOCK) {
		debug_assert(gpa->local_page);
		debug_assert(val == __local_gpa_to_page_len(vmr, gpa));
		__debug_update_rss_add(vmr, gpa->local_page, file, line);
	} else {
		struct emp_gpa *g;
		debug_assert(gpa == emp_get_block_head(gpa));
		debug_assert(mode == DEBUG_UPDATE_RSS_BLOCK);
		debug_assert(val == __local_block_to_page_len(vmr, gpa));
		for_each_gpas(g, gpa) {
			debug_assert(g->local_page);
			__debug_update_rss_add(vmr, g->local_page, file, line);
		}
	}
}
EXPORT_SYMBOL(debug_update_rss_add);

void COMPILER_DEBUG
debug_update_rss_sub(struct emp_vmr *vmr, long val, int ID, int by_emp,
			struct emp_gpa *gpa, int mode, char *file, int line)
{
	debug_assert(val >= 0);
	if (by_emp) {
		debug_assert(ID < NUM_DEBUG_RSS_SUB_ID);
		atomic_long_add(val, &vmr->debug_rss_sub[ID]);
		atomic_long_add(val, &vmr->debug_rss_sub_total);
	} else {
		debug_assert(ID < NUM_DEBUG_RSS_SUB_KERNEL_ID);
		atomic_long_add(val, &vmr->debug_rss_sub_kernel[ID]); \
		atomic_long_add(val, &vmr->debug_rss_sub_kernel_total); \
	}

	if (mode == DEBUG_UPDATE_RSS_SUBBLOCK) {
		debug_assert(gpa->local_page);
		debug_assert(val == __local_gpa_to_page_len(vmr, gpa));
		__debug_update_rss_sub(vmr, gpa->local_page, file, line);
	} else {
		struct emp_gpa *g;
		debug_assert(gpa == emp_get_block_head(gpa));
		debug_assert(mode == DEBUG_UPDATE_RSS_BLOCK);
		debug_assert(val == __local_block_to_page_len(vmr, gpa));
		for_each_gpas(g, gpa) {
			debug_assert(g->local_page);
			__debug_update_rss_sub(vmr, g->local_page, file, line);
		}
	}
}
EXPORT_SYMBOL(debug_update_rss_sub);

void debug_update_rss_init_local_page(struct local_page *lp)
{
	int i;
	for (i = 0; i < DEBUG_RSS_BITMAP_U64LEN; i++)
		lp->debug_rss_bitmap[i] = 0ULL;
#ifdef CONFIG_EMP_DEBUG_RSS_PROGRESS
	for (i = 0; i < DEBUG_RSS_PROGRESS_SIZE; i++) {
		lp->debug_rss_progress[i].file = NULL;
		lp->debug_rss_progress[i].line = -1;
		lp->debug_rss_progress[i].vmr_id = -1;
		lp->debug_rss_progress[i].is_add = -1;
	}
	lp->debug_rss_progress_next = 0;
#endif
}

void debug_update_rss_free_local_page(struct local_page *lp)
{
	int i;
	for (i = 0; i < DEBUG_RSS_BITMAP_U64LEN; i++) {
		if (lp->debug_rss_bitmap[i] != 0ULL)
			goto error_found;
	}
	return;

error_found:
	printk(KERN_ERR "[DEBUG_RSS] ERROR: free_local_page with remained RSS. lp: %016lx gpa_index: %ld\n",
				(unsigned long) lp, lp->gpa_index);
	if (DEBUG_RSS_BITMAP_U64LEN == 1) {
		printk(KERN_ERR "[DEBUG_RSS] (bitmap) lp: %016lx bitmap: %016llx\n",
					(unsigned long) lp, lp->debug_rss_bitmap[0]);
	} else if (DEBUG_RSS_BITMAP_U64LEN == 2) {
		printk(KERN_ERR "[DEBUG_RSS] (bitmap) lp: %016lx bitmap: %016llx %016llx\n",
					(unsigned long) lp, lp->debug_rss_bitmap[0],
					lp->debug_rss_bitmap[1]);
	} else {
		for (i = 0; i < DEBUG_RSS_BITMAP_U64LEN; i++) {
			printk(KERN_ERR "[DEBUG_RSS] (bitmap) lp: %016lx bitmap[%d]: %016llx\n",
					(unsigned long) lp, i, lp->debug_rss_bitmap[i]);
		}
	}
	__debug_update_rss_show_progress(lp);
	BUG();
}
#endif /* CONFIG_EMP_DEBUG_RSS */

/**************************** EMP DEBUG PAGE REF ************************/
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
void __debug_page_ref_print_all(struct local_page *lp)
{
	int i = lp->debug_page_ref_next;
	int num = 0;
	struct debug_page_ref_history *history;
	printk(KERN_ERR "DEBUG: [PAGE_REF_PRINT] lp: %016lx page_len: %d vmr: %d gpa: %lx wrong: %d mmu: %d io: %d will_pte: %d unmap: %d dup: %d calibrate: %d next: %d\n",
			(unsigned long) lp,
			lp->debug_page_ref_page_len,
			lp->vmr_id,
			lp->gpa_index,
			lp->debug_page_ref_wrong,
			lp->debug_page_ref_in_mmu_noti,
			lp->debug_page_ref_in_io,
			lp->debug_page_ref_in_will_pte,
			lp->debug_page_ref_in_unmap,
			lp->debug_page_ref_in_dup,
			lp->debug_page_ref_in_calibrate,
			lp->debug_page_ref_next);
	do {
		history = &lp->debug_page_ref_history[i];
		if (history->file == NULL)
			goto next;
		printk(KERN_ERR "DEBUG: [PAGE_REF_PRINT] lp: %016lx "
				"(%02d) time: 0x%llx add: %2d aggr: %2d refcnt: %2d map: %2d #pmd: %d vmr: %2d mmu: %d io: %d pte: %d unmap: %d dup: %d calibrate: %d %s:%d\n",
				(unsigned long) lp, num,
				history->timestamp,
				history->add, history->sum,
				history->curr, history->map, history->num_pmds,
				history->vmr_id,
				history->in_mmu_noti, history->in_io,
				history->in_will_pte, history->in_unmap,
				history->in_dup,
				history->in_calibrate,
				history->file, history->line);
		num++;
next:
		i = (i + 1) % DEBUG_PAGE_REF_SIZE;
	} while (i != lp->debug_page_ref_next);
}
EXPORT_SYMBOL(__debug_page_ref_print_all);

void debug_check_page_map_status(struct emp_vmr *vmr, struct emp_gpa *head,
			unsigned long head_idx, pmd_t *pmd, bool map_expected)
{
	unsigned long hva;
	unsigned long page_len;
	struct emp_gpa *gpa;
	unsigned long subidx, idx;
	unsigned long _hva, i;
	pte_t *_pte, *pte;

	if (pmd_none(*pmd)) {
		if (map_expected)
			BUG();
		else
			return; // ok
	}

	for_each_gpas_index(gpa, subidx, head) {
		idx = head_idx + subidx;
		____gpa_to_hva_and_len(vmr, gpa, idx, hva, page_len);
		pte = pte_offset_map(pmd, hva);
		for (i = 0, _pte = pte, _hva = hva;
				i < page_len;
				i++, _pte++, _hva += PAGE_SIZE) {
			if (pte_pfn(*_pte))
				BUG_ON(!map_expected);
			else
				BUG_ON(map_expected);
		}
		pte_unmap(pte);
	}
}
#endif

/**************************** EMP DEBUG GPA STATE ***********************/
#ifdef CONFIG_EMP_DEBUG_SHOW_GPA_STATE
const static char *__r_state_str[] = { "REMOT", "ACTIV", "INACT", "WB", "FETCH",
					"TR_IL", "TR_AL", "TR_PL"};
#define __get_r_state_str(r) (((r) < 0 || (r) >= GPA_STATE_MAX) \
					? "UNKNO" : __r_state_str[r])

static inline unsigned long __gpa_offset_to_hva(struct emp_vmr *vmr, unsigned long gpa_offset) {
	return __gfn_offset_to_hva(vmr, gpa_offset, bvma_subblock_order(vmr->emm));
}

static inline unsigned long
__get_pte_val(struct emp_vmr *vmr, pmd_t *pmdp, unsigned long hva)
{
	pte_t *ptep;
	/* NOTE: don't touch *pmdp if vmr->vmr_closing == true. */
	if (vmr->vmr_closing || pmdp == NULL || pmd_none(*pmdp) || hva == 0UL)
		return 0;
	ptep = pte_offset_map(pmdp, hva);
	return ptep ? ((unsigned long) ptep->pte) : 0UL;
}

inline void
__debug_show_gpa_state(struct emp_vmr *vmr, struct emp_gpa *gpa,
					unsigned long idx, bool head)
{
	static struct local_page empty_local_page = {
		.lru_list = { .prev = &empty_local_page.lru_list,
				.next = &empty_local_page.lru_list},
		.page = NULL,
		.sptep = NULL,
		.flags = 0,
		.gpa_index = -1,
		.dma_addr = 0,
		.vmr_id = -1,
		.pmds = { .next = NULL, .pmd = NULL, .vmr_id = -1 },
	};
	struct local_page *local_page = (gpa && gpa->local_page)
					? gpa->local_page : &empty_local_page;
	struct page *page = local_page->page;
	int lp_pmd_count = (gpa && gpa->local_page) ? emp_lp_count_pmd(gpa->local_page) : 0;

	/* printed information
	 * offset: index of gpa descriptor
	 * ptr: pointer address of gpa descriptor
	 * flag: flag of gpa descriptor
	 * state: r_state of gpa descriptor
	 * ref: reference count of gpa descriptor
	 * order: (block order)-(subblock order)
	 * remote_page: value of remote page
	 * ref: reference count of remote_page (for CoW remote page)
	 * lru: is this gpa descrioptor is inserted to any lru chains?
	 * flag: flag of local page
	 * vmr: vmr id of the local page (local_page->vmr_id)
	 * page_struct: address of the page structure
	 * page_flag: flags of the page structure
	 * page_addr: kernel virtual address of the page
	 * page_pfn: physical frame number (pfn) of the page
	 * comp: is the page compound page?
	 * ref: reference count of the page
	 * map: map count of the page
	 * cnt: the number of inserted pmd to local page
	 * vmr: the first inserted vmr id of the local page
	 * hva: host virtual address of gpa
	 * pmd: the first inserted pmd of the local page
	 * pte: the first inserted pmd of the local page
	 *
	 * if cnt > 1, the following lines print out vmr, pmd, pte of the additional mappings.
	 */
	if (head)
		printk("%6s %16s %8s %5s %3s %5s " // 49
			"%16s %3s %3s %4s %3s " // 34
			"%16s %16s %16s %11s %4s %3s %3s %3s " // 60
			"%3s %16s %16s %16s\n", // 54
			"offset", "ptr", "flag", "state", "ref", "order",
			"remote_page", "ref", "lru", "flag", "vmr",
			"page_struct", "page_flag", "page_addr", "page_pfn", "comp", "ref", "map", "cnt",
			"vmr", "hva", "pmd", "pte");
	if (gpa) {
		pmd_t *pmdp = local_page->pmds.pmd;
		unsigned long addr = page ? (unsigned long) page_address(page) : 0UL;
		unsigned long hva = __gpa_offset_to_hva(vmr, idx);
		unsigned long pte = __get_pte_val(vmr, pmdp, hva);
		printk("%06lx %016lx %08x %5s %3d %3d-%1d "
			"%016llx %3d %3c %04x %3d "
			"%016lx %016lx %016lx %11lx %4c %3d %3d %3d "
			"%3d %016lx %016lx %016lx\n",

			idx,
			(unsigned long) gpa,
			gpa->flags,
			__get_r_state_str(gpa->r_state),
			atomic_read(&gpa->refcnt),
			gpa->block_order, gpa->sb_order,

			get_gpa_remote_page_val(gpa),
			get_gpa_remote_page_refcnt(gpa),
			list_empty(&local_page->lru_list) ? 'X' : 'O',
			local_page->flags,
			local_page->vmr_id,

			(unsigned long) page,
			page ? page->flags : 0L,
			addr,
			page ? page_to_pfn(page) : 0UL,
			page ? (PageCompound(page) ? 'O' : 'X') : 'N',
			page ? page_ref_count(page) : -1,
			page ? page_mapcount(page) : -1,
			lp_pmd_count,

			local_page->pmds.vmr_id,
			hva,
			(unsigned long) pmdp,
			pte);
		if (lp_pmd_count > 1) {
			struct mapped_pmd *p = local_page->pmds.next;
			struct emp_vmr *vmr_next;
			int num = 1;
			BUG_ON(p == NULL); /* because lp_pmd_count > 1 */
			while (p != &local_page->pmds) {
				vmr_next = vmr->emm->vmrs[p->vmr_id];
				hva = __gpa_offset_to_hva(vmr_next, idx);
				printk("%159s%02d: %3d %016lx %016lx %016lx\n",
						"pmd", num,
						p->vmr_id,
						hva,
						(unsigned long) p->pmd,
						__get_pte_val(vmr_next, p->pmd, hva));
				p = p->next;
			}
		}
	}
}
EXPORT_SYMBOL(__debug_show_gpa_state);

void debug_show_gpa_state(struct emp_vmr *vmr, const char *func)
{
	struct emp_vmdesc *desc = vmr->descs;
	struct emp_mm *emm = vmr->emm;
	unsigned long i, gpa_len;

	if (unlikely(!desc))
		return;

	if (desc->gpa_len > CONFIG_EMP_DEBUG_SHOW_GPA_STATE)
		return;

	emp_debug_bulk_msg_lock();
	BUG_ON(!vmr || !emm);
	printk(KERN_ERR "[SHOW_GPA] (%d-%d) from: %s%s\n", emm->id, vmr->id, func,
						vmr->vmr_closing ? " (closing)" : "");
	if (unlikely(!desc)) {
		printk(KERN_ERR "[SHOW_GPA] (%d-%d) ERROR: vmdesc is null\n", emm->id, vmr->id);
		emp_debug_bulk_msg_unlock();
		return;
	}

	gpa_len = desc->gpa_len;
	printk(KERN_ERR "[SHOW_GPA] (%d-%d) VMDESC(0x%lx) refcount: %d gpa_len: 0x%lx vm_base: 0x%lx block_aligned_start: 0x%lx gpa_dir_alloc_size: 0x%lx\n",
			emm->id, vmr->id, (unsigned long) desc,
			atomic_read(&desc->refcount), gpa_len,
			desc->vm_base, desc->block_aligned_start,
			desc->gpa_dir_alloc_size);

	for (i = 0; i < gpa_len; i++)
		__debug_show_gpa_state(vmr, desc->gpa_dir[i], i,
							(i & 0xf) == 0);

	emp_debug_bulk_msg_unlock();
}
EXPORT_SYMBOL(debug_show_gpa_state);
#endif

/**************************** EMP DEBUG ALLOC ***************************/
#ifdef CONFIG_EMP_DEBUG_ALLOC
/* to align dbg_alloc_data on a cache line */
#define DBG_ALLOC_DATA_FILE_MAX (32 - sizeof(size_t) - sizeof(int))
struct dbg_alloc_data {
	size_t size;
	int line;
	char file[DBG_ALLOC_DATA_FILE_MAX];
};

struct dbg_alloc_table {
	spinlock_t lock;
	unsigned long *addr;
	struct dbg_alloc_data *data;
	unsigned long size; /* the number of allocated entries of data */
	unsigned long next; /* the first free entry index of data */
	/* to align on a cache line */
	char pad[64 - sizeof(unsigned long) - sizeof(unsigned long)
		- sizeof(struct dbg_alloc_data *) - sizeof(spinlock_t)];
};

/* For alloc/free of memory */
#define DBG_ALLOC_DATA_NUM_INC (1UL << 14)
#define DBG_ALLOC_TABLE_BITS (15)
#define DBG_ALLOC_TABLE_SHIFT (4)
#define DBG_ALLOC_TABLE_NUM (1UL << DBG_ALLOC_TABLE_BITS)
#define dbg_alloc_get_tid(addr) (((addr) >> DBG_ALLOC_TABLE_SHIFT) \
					& ((1UL << DBG_ALLOC_TABLE_BITS) - 1))
static struct dbg_alloc_table dbg_alloc_table[DBG_ALLOC_TABLE_NUM];


/* For kmem_cache_create/destroy */
#define DBG_CACHE_DATA_NUM_INC (32UL)
#define DBG_CACHE_TABLE_BITS (5)
#define DBG_CACHE_TABLE_SHIFT (5)
#define DBG_CACHE_TABLE_NUM (1UL << DBG_CACHE_TABLE_BITS)
#define dbg_cache_get_tid(addr) (((addr) >> DBG_CACHE_TABLE_SHIFT) \
					& ((1UL << DBG_CACHE_TABLE_BITS) - 1))

static struct dbg_alloc_table dbg_cache_table[DBG_CACHE_TABLE_NUM];

atomic64_t emp_debug_alloc_size_aggr;
atomic64_t emp_debug_alloc_size_max;
atomic64_t emp_debug_alloc_size_alloc;
atomic64_t emp_debug_alloc_size_free;

static inline void __debug_alloc_size_init(void) {
	atomic64_set(&emp_debug_alloc_size_aggr, 0);
	atomic64_set(&emp_debug_alloc_size_max, 0);
	atomic64_set(&emp_debug_alloc_size_alloc, 0);
	atomic64_set(&emp_debug_alloc_size_free, 0);
}

static inline void __debug_alloc_size_add(size_t size)
{
	s64 aggr = atomic64_add_return(size, &emp_debug_alloc_size_aggr);
	s64 max = atomic64_read(&emp_debug_alloc_size_max);
	atomic64_add(size, &emp_debug_alloc_size_alloc);
	if (aggr > max) {
		int num_try = 0;
		s64 old;
retry:
		old = atomic64_cmpxchg(&emp_debug_alloc_size_max, max, aggr);
		if (aggr <= old)
			return;

		if (++num_try < 1000) {
			max = old;
			goto retry;
		}
		printk(KERN_ERR "[EMP_ALLOC] ERROR: failed to update size_max."
				" old_max: %lld new_max: %lld num_tries: %d\n",
				old, aggr, num_try);
	}
}

static inline void __debug_alloc_size_sub(size_t size)
{
	if (size == 0) return;
	atomic64_sub(size, &emp_debug_alloc_size_aggr);
	atomic64_add(size, &emp_debug_alloc_size_free);
}

static void __debug_alloc_table_init(struct dbg_alloc_table *table_arr,
					unsigned long num_table,
					unsigned long inc_data,
					const char *name)
{
	unsigned long tid;
	unsigned long total_table, total_entry;
	struct dbg_alloc_table *table;

	total_table = 0;
	total_entry = 0;
	for (tid = 0; tid < num_table; tid++) {
		table = &table_arr[tid];
		spin_lock_init(&table->lock);
		spin_lock(&table->lock);
		table->size = 0;
		table->next = 0;
		table->addr = vzalloc(inc_data * sizeof(unsigned long));
		table->data = vzalloc(inc_data * sizeof(struct dbg_alloc_data));
		if (table->addr && table->data) {
			table->size = inc_data;
			total_table++;
		} else {
			if (table->addr) vfree(table->addr);
			if (table->data) vfree(table->data);
			printk(KERN_ERR "[EMP_ALLOC] ERROR: failed to allocate %s_table%ld\n",
					name, tid);
		}
		total_entry += table->size;
		spin_unlock(&table->lock);
		cond_resched(); /* for each table */
	}
	printk(KERN_ERR "[EMP_ALLOC] INFO: allocate %ld %s_tables (%ld entries in total)\n",
				total_table, name, total_entry);
}

static void __debug_alloc_table_exit(struct dbg_alloc_table *table_arr,
					unsigned long num_table,
					const char *name,
					const char *errtype)
{
	unsigned long tid;
	unsigned long idx;
	unsigned long num_error = 0;
	struct dbg_alloc_table *table;
	struct dbg_alloc_data *data;
	unsigned long addr;

	printk(KERN_ERR "[EMP_ALLOC] debug %s checking starts\n", name);
	for (tid = 0; tid < num_table; tid++) {
		table = &table_arr[tid];
		spin_lock(&table->lock);
		if (table->size == 0) {
			printk(KERN_ERR "[EMP_ALLOC] WARN: %s_table%ld has no entry."
					" It may have errors.\n", name, tid);
			goto next;
		}

		for (idx = 0; idx < table->next; idx++) {
			if (idx % 1024 == 0) /* first and each 1024 entries */
				cond_resched();
			addr = table->addr[idx];
			data = &table->data[idx];
			if (addr == 0)
				continue;

			printk(KERN_ERR "[EMP_ALLOC] ERROR: %s addr: %016lx size: %16ld"
					" allocated at %s:%d\n",
					errtype, addr, data->size,
					data->file, data->line);
			num_error++;
		}

		if (table->size > 0) {
			vfree(table->addr);
			vfree(table->data);
			table->addr = NULL;
			table->data = NULL;
			table->size = 0;
		}
		table->next = 0;
next:
		spin_unlock(&table->lock);
	}

	printk(KERN_ERR "[EMP_ALLOC] debug %s checking ends. num_error: %ld\n",
			name, num_error);

}

void emp_debug_alloc_init(void)
{
	__debug_alloc_size_init();
	__debug_alloc_table_init(dbg_alloc_table,
				DBG_ALLOC_TABLE_NUM,
				DBG_ALLOC_DATA_NUM_INC,
				"alloc");
	__debug_alloc_table_init(dbg_cache_table,
				DBG_CACHE_TABLE_NUM,
				DBG_CACHE_DATA_NUM_INC,
				"cache");
}

void emp_debug_alloc_exit(void)
{
	__debug_alloc_table_exit(dbg_alloc_table,
				DBG_ALLOC_TABLE_NUM,
				"alloc",
				"NOT_FREED");
	__debug_alloc_table_exit(dbg_cache_table,
				DBG_CACHE_TABLE_NUM,
				"cache",
				"NOT_DESTROYED");
	printk(KERN_ERR "[EMP_ALLOC] exit size_aggr: %lld size_max: %lld"
			" size_alloc: %lld size_free: %lld\n",
			atomic64_read(&emp_debug_alloc_size_aggr),
			atomic64_read(&emp_debug_alloc_size_max),
			atomic64_read(&emp_debug_alloc_size_alloc),
			atomic64_read(&emp_debug_alloc_size_free));
}

static inline
void __debug_alloc_data_add(const char *name, const int num_inc,
			struct dbg_alloc_table *table, unsigned long tid,
			unsigned long addr, size_t size,
			const char *file, const int line)
{
	struct dbg_alloc_data *data;
	size_t file_len = 0;

	spin_lock(&table->lock);
	if (unlikely(table->size == 0)) {
		printk(KERN_ERR "[EMP_ALLOC] WARN: %s called with addr: %016lx"
				" but %s_table%ld has no entries."
				" It may have errors.\n",
				__func__, addr, name, tid);
		goto out;
	}

	if (unlikely(table->next == table->size)) {
		unsigned long new_size;
		unsigned long *new_addr;
		struct dbg_alloc_data *new_data;
		new_size = table->size + num_inc;
		new_addr = vzalloc(new_size * sizeof(unsigned long));
		new_data = vzalloc(new_size * sizeof(struct dbg_alloc_data));

		if (!new_addr || !new_data) {
			if (new_addr) vfree(new_addr);
			if (new_data) vfree(new_data);
			vfree(table->addr);
			vfree(table->data);
			table->addr = NULL;
			table->data = NULL;
			table->size = 0;
			table->next = 0;
			goto out;
		}
		memcpy(new_addr, table->addr,
				table->next * sizeof(unsigned long));
		vfree(table->addr);
		memcpy(new_data, table->data,
				table->next * sizeof(struct dbg_alloc_data));
		vfree(table->data);
		table->addr = new_addr;
		table->data = new_data;
		printk(KERN_ERR "%s: the size of %s_table[%ld] increases %ld -> %ld\n",
				__func__, name, tid,
				table->size, new_size);
		table->size = new_size;
	}

	table->addr[table->next] = addr;
	data = &table->data[table->next];
	data->size = size;
	data->line = line;
	file_len = strlen(file);
	if (unlikely(file_len >= DBG_ALLOC_DATA_FILE_MAX)) {
		strlcpy(data->file, file + file_len - DBG_ALLOC_DATA_FILE_MAX + 1,
							DBG_ALLOC_DATA_FILE_MAX);
		if (DBG_ALLOC_DATA_FILE_MAX >= 4) { /* enough memory for "..." */
			data->file[0] = '.';
			data->file[1] = '.';
			data->file[2] = '.';
		}
	} else
		strlcpy(data->file, file, DBG_ALLOC_DATA_FILE_MAX);
	table->next++;
out:
	spin_unlock(&table->lock);
}

#define DO_REMOVE 1
#define DONT_REMOVE 0

static inline
size_t __debug_alloc_data_search(const char *name, const char *errtype,
			struct dbg_alloc_table *table, unsigned long tid,
			unsigned long addr, const char *file, const int line,
			const int mode)
{
	unsigned long idx, rem;
	struct dbg_alloc_data *data;
	size_t size = 0;

	spin_lock(&table->lock);
	if (table->size == 0) {
		printk(KERN_ERR "[EMP_ALLOC] WARN: %s called with addr: %lx"
				" but %s_table%ld has no entries."
				" It may have errors.\n",
				__func__, addr, name, tid);
		goto out;
	}

	/* Finding addr in the table is the main bottleneck of EMP_DEBUG_ALLOC.
	 * It significantly affects the performance.
	 * Thus, we use some tricks.
	 * In the first loop, we search the address for 8 entries with only 2 branches.
	 * (check index range check and check address membership in the 8 entries.)
	 * If the address is found, the second loop quickly gives the exact index.
	 * Otherwise, go to the next 8 entries.
	 *
	 * If the number of remaining entries is less than 8,
	 * the second loop also works for them.
	 */
	idx = 0;
	rem = table->next;
	while (rem >= 8) {
		/* Note that !(a ^ b) = 1 if a == b, 0 otherwise.
		 * The number in "if" gives the number of entries which have @addr.
		 */
		if ((!(table->addr[idx] ^ addr))
				+ (!(table->addr[idx + 1] ^ addr))
				+ (!(table->addr[idx + 2] ^ addr))
				+ (!(table->addr[idx + 3] ^ addr))
				+ (!(table->addr[idx + 4] ^ addr))
				+ (!(table->addr[idx + 5] ^ addr))
				+ (!(table->addr[idx + 6] ^ addr))
				+ (!(table->addr[idx + 7] ^ addr)))
			break;
		idx = idx + 8;
		rem -= 8;
	}

	/* The remaining entries */
	while (rem > 0) {
		if (table->addr[idx] == addr)
			goto found;
		idx++;
		rem--;
	}

	/* addr is not found, or table has no data (rem == 0) */
	printk(KERN_ERR "[EMP_ALLOC] ERROR: %s addr: %lx"
			" called at %s:%d\n",
			errtype, addr, file, line);
	spin_unlock(&table->lock);
	return 0;

found:
	data = &table->data[idx];
	size = data->size; /* for return */

	if (mode == DONT_REMOVE)
		goto out;

	if (idx == table->next - 1) {
		/* just reset @addr[idx] and @data[idx] */
		table->addr[idx] = 0;
		memset(data, 0, sizeof(struct dbg_alloc_data));
	} else {
		/* copy the last valid addr and data to @addr[idx] and @data[idx] */
		struct dbg_alloc_data *last;
		table->addr[idx] = table->addr[table->next - 1];
		last = &table->data[table->next - 1];
		memcpy(data, last, sizeof(struct dbg_alloc_data));
		memset(last, 0, sizeof(struct dbg_alloc_data));
	}
	table->next--;
out:
	spin_unlock(&table->lock);

	return size;
}

void __debug_alloc(void *__addr, size_t size, const char *file, const int line)
{
	unsigned long addr = (unsigned long) __addr;
	unsigned long tid;
	struct dbg_alloc_table *table;

	if (addr == 0 || size == 0) return;

	tid = dbg_alloc_get_tid(addr);
	table = &dbg_alloc_table[tid];

	__debug_alloc_data_add("alloc", DBG_ALLOC_DATA_NUM_INC,
				table, tid, addr, size, file, line);
	__debug_alloc_size_add(size);
}
EXPORT_SYMBOL(__debug_alloc);

void __debug_free(void *__addr, const char *file, const int line)
{
	unsigned long addr = (unsigned long) __addr;
	unsigned long tid;
	struct dbg_alloc_table *table;
	size_t size = 0;

	if (!addr) return;

	tid = dbg_alloc_get_tid(addr);
	table = &dbg_alloc_table[tid];

	size = __debug_alloc_data_search("alloc", "DOUBLE-FREE",
					table, tid,
					addr, file, line,
					DO_REMOVE);
	__debug_alloc_size_sub(size);
}
EXPORT_SYMBOL(__debug_free);

void __debug_cache_create(void *__addr, size_t size, const char *file, const int line)
{
	unsigned long addr = (unsigned long) __addr;
	unsigned long tid;
	struct dbg_alloc_table *table;

	if (addr == 0 || size == 0) return;
	tid = dbg_cache_get_tid(addr);
	table = &dbg_cache_table[tid];

	__debug_alloc_data_add("cache", DBG_CACHE_DATA_NUM_INC,
				table, tid, addr, size, file, line);
}
EXPORT_SYMBOL(__debug_cache_create);

size_t __debug_cache_get_size(void *__addr, const char *file, const int line)
{
	unsigned long addr = (unsigned long) __addr;
	unsigned long tid;
	struct dbg_alloc_table *table;

	if (!addr) return 0;

	tid = dbg_cache_get_tid(addr);
	table = &dbg_cache_table[tid];

	return __debug_alloc_data_search("cache", "NOT-FOUND",
					table, tid,
					addr, file, line,
					DONT_REMOVE);
}
EXPORT_SYMBOL(__debug_cache_get_size);

void __debug_cache_destroy(void *__addr, const char *file, const int line)
{
	unsigned long addr = (unsigned long) __addr;
	unsigned long tid;
	struct dbg_alloc_table *table;

	if (!addr) return;

	tid = dbg_cache_get_tid(addr);
	table = &dbg_cache_table[tid];

	__debug_alloc_data_search("cache", "DOUBLE-DESTROY",
				table, tid,
				addr, file, line,
				DO_REMOVE);
}
EXPORT_SYMBOL(__debug_cache_destroy);
#endif /* CONFIG_EMP_DEBUG_ALLOC */

/**************************** EMP DEBUG INTEGRITY ***************************/
#ifdef CONFIG_EMP_DEBUG_INTEGRITY

#define PRINT_CHECK_TAG_STEP (10000)

static inline u64 __generate_tag_aligned(u64 *addr, u64 *end)
{
	u64 tag0 = 0, tag1 = 0, tag2 = 0, tag3 = 0,
		tag4 = 0, tag5 = 0, tag6 = 0, tag7 = 0;
	while (addr < end) {
		tag0 ^= *(addr + 0);
		tag1 ^= *(addr + 1);
		tag2 ^= *(addr + 2);
		tag3 ^= *(addr + 3);
		tag4 ^= *(addr + 4);
		tag5 ^= *(addr + 5);
		tag6 ^= *(addr + 6);
		tag7 ^= *(addr + 7);
		addr += 8;
	}
	return tag0 ^ tag1 ^ tag2 ^ tag3 ^ tag4 ^ tag5 ^ tag6 ^ tag7;
}

// return the tag value
static u64 __generate_tag(void *addr_start, void *addr_end, u64 tag)
{
	u64 *addr = (u64 *) addr_start;
	u64 *end = (u64 *) addr_end;

	if (likely(end - addr >= 8)) {
		u64 *end_aligned = end - ((end - addr) % 8);
		tag ^= __generate_tag_aligned(addr, end_aligned);
		addr = end_aligned;
	}

	if (unlikely(addr < end)) {
		do {
			tag = tag ^ *addr;
			addr++;
		} while (addr < end);
	}
	return tag;
}

static inline void *__get_addr_start_to_decrypt(struct work_request *w)
{
	return w->addr_upto ? w->addr_upto
			: page_address(w->gpa->local_page->page);
}

static inline void *__get_addr_end_of_gpa(struct emp_gpa *gpa, void *start)
{
	return (void *) ((char *) start
				+ gpa_subblock_size(gpa) * PAGE_SIZE);
}

static inline int __check_tag(struct emp_gpa *gpa, u64 tag_calculated) {
	static atomic64_t num_correct = {0};
	static atomic64_t num_breaks = {0};
	if (likely(gpa->tag == tag_calculated)) {
		s64 num_ok = atomic64_add_return(1, &num_correct);
		if (unlikely(num_ok % PRINT_CHECK_TAG_STEP == 0)) {
			s64 num_br = atomic64_read(&num_breaks);
			printk(KERN_ERR "[debug_integrity] INFO: integrity total_check: %lld total_break: %lld\n",
					num_ok, num_br);
		}
		return 0;
	} else {
		atomic64_inc(&num_breaks);
		printk(KERN_ERR "[debug_integrity] ERROR: integrity breaks gpa: %016llx"
				" page_address: %016llx"
				" tag(stored): %016llx tag(calculated): %016llx"
				" total_check: %lld total_breaks: %lld\n",
			(unsigned long long) gpa,
			(unsigned long long) page_address(gpa->local_page->page),
			gpa->tag, tag_calculated,
			atomic64_read(&num_correct),
			atomic64_read(&num_breaks));
		return -1;
	}
}

// return 0 if tag is matched
// return -1 if tag is not matched
int debug_check_tag(struct work_request *w)
{
	void *addr_start = __get_addr_start_to_decrypt(w);
	void *addr_end = __get_addr_end_of_gpa(w->gpa, addr_start);
	u64 tag = __generate_tag(addr_start, addr_end, w->tag_upto);
	return __check_tag(w->gpa, tag);
}

// return 0 if upto is not at the end or tag is matched
// return -1 if upto is at the end and tag is not matched
int debug_check_tag_upto(struct work_request *w, void *upto)
{
	void *addr_start = __get_addr_start_to_decrypt(w);
	void *addr_end = __get_addr_end_of_gpa(w->gpa, addr_start);

	if (upto == addr_end) {
		u64 tag = __generate_tag(addr_start, addr_end, w->tag_upto);
		return __check_tag(w->gpa, tag);
	}
	w->tag_upto = __generate_tag(addr_start, addr_end, w->tag_upto);
	w->addr_upto = upto;
	return 0;
}

void debug_generate_tag(struct emp_gpa *gpa)
{
	void *addr_start = page_address(gpa->local_page->page);
	void *addr_end = __get_addr_end_of_gpa(gpa, addr_start);
	gpa->tag = __generate_tag(addr_start, addr_end, 0);
}

#endif /* CONFIG_EMP_DEBUG_INTEGRITY */

/************************************************************************/

/* get offset between gpn and gfn  */
static inline u64 get_gfn_offset(struct emp_gpa *g) {
	BUG_ON(IS_INVALID_GFN_OFFSET(g->gfn_offset_order));
	return g->gfn_offset_order > 1? 
		1 << (g->gfn_offset_order - 1): 0;
}

void debug_emp_unlock_block(struct emp_gpa *head) {
	BUG_ON(!____emp_gpa_is_locked(head));
	if (head->r_state == GPA_ACTIVE
			|| head->r_state == GPA_INACTIVE) {
		struct local_page *lp = head->local_page;
		debug_assert(lp);
		debug_assert(lp->vmr_id >= 0);
		debug_assert(is_local_page_on_list(lp));
		debug_assert(!list_empty(&lp->lru_list));
		debug_assert(lp->lru_list.next != LIST_POISON1
				&& lp->lru_list.prev != LIST_POISON2);
	} else if (head->r_state == GPA_WB) {
		struct local_page *lp = head->local_page;
		struct work_request *w;
		debug_assert(lp);
		debug_assert(is_local_page_on_list(lp));
		w = lp->w;
		debug_assert(w);
		debug_assert(!list_empty(&w->sibling));
		debug_assert(w->sibling.next != LIST_POISON1
				&& w->sibling.prev != LIST_POISON2);
	} else if (head->r_state == GPA_INIT) {
		debug_assert(!head->local_page);
	} else if (head->r_state == GPA_FETCHING) {
		printk(KERN_ERR "WARN: gpa will be unlocked with "
				"r_state = GPA_FETCHING. flags: 0x%x\n",
				head->flags);
		BUG();
	} else
		BUG();
}
EXPORT_SYMBOL(debug_emp_unlock_block);

void debug_check_head(struct page *page) {
	BUG_ON(PageCompound(page) && !PageHead(page));
}

void debug__handle_gpa_on_inactive_fault(struct emp_gpa *head)
{
	int i;
	struct emp_gpa *g;
	struct page *p;

	for_each_gpas(g, head) {
		p = g->local_page->page;
		for (i = 0; i < gpa_subblock_size(g); i++) {
			if (page_count(p + i) != 1)
				printk(KERN_ERR "WARN: %s page count is not matched. "
						"page_count(%016lx): %d != 1\n",
						__func__, (unsigned long)(p + i),
						page_count(p + i));
		}
	}
}

void debug_fetch_block(struct emp_gpa *head, int fip)
{
	struct emp_gpa *g;
	struct page *p;
	int i, expected_count;

	if (fip < 0) /* error case */
		return;

	for_each_gpas(g, head) {
		p = g->local_page->page;
		expected_count = 1;
		if (fip)
			expected_count++;
		for (i = 0; i < gpa_subblock_size(g); i++) {
			if (page_count(p + i) != expected_count)
				printk(KERN_ERR "WARN: %s page count is not matched. "
						"page_count(%016lx): %d != %d\n",
						__func__, (unsigned long)(p + i),
						page_count(p + i), expected_count);
		}
	}
}

#ifdef CONFIG_EMP_VM
void debug_map_spte(struct page *p) {
	BUG_ON(!PageCompound(p) && page_count(p) < 2);
}

void debug_check_sptep(u64 *sptep) {
	BUG_ON(sptep == NULL);
}

void debug_emp_map_prefetch_sptes2(struct emp_gpa *g, struct emp_gpa *eg) {
	BUG_ON(!g->local_page);
	BUG_ON(eg < g);
}

void debug_emp_page_fault_gpa(struct emp_gpa *head) {
#ifdef CONFIG_EMP_VM
	BUG_ON(is_gpa_flags_set(head, GPA_HPT_MASK) &&
			is_gpa_flags_set(head, GPA_EPT_MASK));
#endif
}

void debug_emp_page_fault_gpa2(struct emp_gpa *head)
{
	struct emp_gpa *g;
	struct page *p;
	int i, sb_head_count = 1;

	if (is_gpa_flags_set(head, GPA_HPT_MASK) || head->r_state == GPA_INIT)
		return;

	if (WB_BLOCK(head))
		sb_head_count++;
	else if (is_gpa_flags_set(head, EAGER_WBR) && INACTIVE_BLOCK(head))
		sb_head_count++;

	for_each_gpas(g, head) {
		p = g->local_page->page;
		for (i = 0; i < gpa_subblock_size(g); i++) {
			if (WB_BLOCK(head) && !g->local_page->w) {
				if (page_count(p + i) != (sb_head_count - 1))
					printk(KERN_ERR "WARN: %s page count is not matched. (writeback) "
							"page_count(%016lx): %d != %d\n",
							__func__, (unsigned long)(p + i),
							page_count(p + i), sb_head_count - 1);
				continue;
			}
			if (INACTIVE_BLOCK(head) &&
					is_gpa_flags_set(head, EAGER_WBR) &&
					is_gpa_flags_set(g, ZERO_BLOCK)) {
				if (page_count(p + i) != (sb_head_count - 1))
					printk(KERN_ERR "WARN: %s page count is not matched. (inactive, eager, zero) "
							"page_count(%016lx): %d != %d\n",
							__func__, (unsigned long)(p + i),
							page_count(p + i), sb_head_count - 1);
				continue;
			}

			if (page_count(p + i) != sb_head_count)
				printk(KERN_ERR "WARN: %s page count is not matched. "
						"page_count(%016lx): %d != %d\n",
						__func__, (unsigned long)(p + i),
						page_count(p + i), sb_head_count);
		}
	}
}

void debug_emp_page_fault_gpa3(struct emp_gpa *h, gva_t gva, pgoff_t o) {
	if (unlikely(IS_INVALID_GFN_OFFSET(h->gfn_offset_order))) {
		unsigned long gvn = GPA_TO_GFN(gva);
		// generally gvn is equal or greater than demand_off(o)
		int order = (gvn == o)? 0: ilog2(gvn - o);
		h->gfn_offset_order = (order + 1);
	}
}

void debug___mmu_set_spte(u64 pfn) {
	BUG_ON(page_count(pfn_to_page(pfn)) < 2);
}

void debug_map_sptes_in_subblock(struct emp_gpa *sb_head, u64 *sptep) {
	BUG_ON(sptep == NULL);
	BUG_ON(sb_head->local_page == NULL);
	BUG_ON(sb_head->local_page->page == NULL);
}

void debug_emp_install_sptes(struct emp_gpa *head)
{
	int i;
	struct emp_gpa *g;
	struct page *p;

	for_each_gpas(g, head) {
		BUG_ON(g->local_page == NULL);
		p = g->local_page->page;
		for (i = 0; i < gpa_subblock_size(g); i++) {
			if (is_gpa_flags_set(head, GPA_HPT_MASK)) {
				if (page_count(p + i) < 3)
					printk(KERN_ERR "WARN: %s page count is not matched. (hpt) "
							"page_count(%016lx): %d < 3\n",
							__func__, (unsigned long)(p + i),
							page_count(p + i));
			} else {
				if (page_count(p + i) < 2)
					printk(KERN_ERR "WARN: %s page count is not matched. (not hpt) "
							"page_count(%016lx): %d < 2\n",
							__func__, (unsigned long)(p + i),
							page_count(p + i));
			}
		}
	}
}

void debug_emp_install_sptes2(struct emp_mm *bvma, struct emp_gpa *head, struct emp_gpa *gpa) {
	struct emp_gpa *g;
	struct page *p;
	int i;
	int demand_page_offset = head->local_page->demand_offset & gpa_subblock_mask(gpa);
	bool cpf_prefetched = is_gpa_flags_set(head, GPA_PREFETCHED_CPF_MASK);

	if (is_gpa_flags_set(head, GPA_PREFETCHED_MASK)) {
		for_each_gpas(g, head) {
			BUG_ON(g->local_page == NULL);
			p = g->local_page->page;
			for (i = 0; i < gpa_subblock_size(g); i++) {
				if (cpf_prefetched && !PageCompound(p + i)) {
					/* cpf_prefetched && page_level count */
					if (g == gpa && demand_page_offset == i) {
						/* prefetched page ==> already mapped */
						if (page_count(p + i) < 1)
							printk(KERN_ERR "WARN: %s page count is not matched. (cpf, not compound, prefetched) "
									"page_count(%016lx): %d < 1\n",
										__func__, (unsigned long)(p + i),
										page_count(p + i));
					} else {
						/* other pages */
						if (page_count(p + i) < 2)
							printk(KERN_ERR "WARN: %s page count is not matched. (cpf, not compound, other) "
									"page_count(%016lx): %d < 2\n",
										__func__, (unsigned long)(p + i),
										page_count(p + i));
					}
				} else {
					/* csf prefetched in any case || cpf prefetched with compound page */
					if (g == gpa) {
						/* prefetched sub-block or compound page ==> already mapped */
						if (page_count(p + i) < 1)
							printk(KERN_ERR "WARN: %s page count is not matched. (csf prefetched or cpf prefetched with compound) "
									"page_count(%016lx): %d < 1\n",
										__func__, (unsigned long)(p + i),
										page_count(p + i));
					} else {
						/* other subblocks or compund pages */
						if (page_count(p + i) < 2)
							printk(KERN_ERR "WARN: %s page count is not matched. (not prefetched or compound) "
									"page_count(%016lx): %d < 2\n",
										__func__, (unsigned long)(p + i),
										page_count(p + i));
					}
				}
			}
		}
	} else {
		int pc = is_gpa_flags_set(head, GPA_HPT_MASK)?2:1;
		for_each_gpas(g, head) {
			BUG_ON(g->local_page == NULL);
			p = g->local_page->page;
			for (i = 0; i < gpa_subblock_size(g); i++) {
				if (page_count(p + i) < pc)
					printk(KERN_ERR "WARN: %s page count is not matched. (normal) "
							"page_count(%016lx): %d < %d\n",
								__func__, (unsigned long)(p + i),
								page_count(p + i), pc);
			}
		}
	}
}
#endif /* CONFIG_EMP_VM */

void debug__alloc_pages(struct page *page, int page_order)
{
	int i;
	for (i = 0; i < (1 << page_order); i++) {
		if (page_count(page + i) != 1)
			printk(KERN_ERR "WARN: %s page count is not matched. "
					"page_count(%016lx): %d != 1\n",
					__func__, (unsigned long)(page + i),
					page_count(page + i));
		if (PageDirty(page + i))
			printk(KERN_ERR "WARN: %s page is dirty. "
					"PageDirty(%016lx): %d\n",
					__func__, (unsigned long)(page + i),
					PageDirty(page + i));
	}
}

#ifdef CONFIG_EMP_BLOCKDEV
#define READ_FROM_BLOCK 0
#define WRITE_TO_BLOCK  1
void debug_emp_bdev_wait_rw(struct work_request *w, int rw)
{
	BUG_ON(!w);
	BUG_ON((rw == READ_FROM_BLOCK) && (w->type == TYPE_WB));
	BUG_ON((rw == WRITE_TO_BLOCK) && (w->type == TYPE_FETCHING));
}
#endif /* CONFIG_EMP_BLOCKDEV */

void debug_alloc_and_fetch_pages(struct emp_mm *bvma, struct emp_gpa *gpa,
		struct page *free_page, int page_order, int avail_dma_order)
{
	BUG_ON(free_page == NULL);

	BUG_ON(page_order > avail_dma_order);
	BUG_ON(gpa->local_page);
}

void debug_alloc_and_fetch_pages2(struct emp_vmr *v, struct emp_gpa *gpa)
{
	BUG_ON(page_count(gpa->local_page->page) > 1 &&
		!is_gpa_flags_set(emp_get_block_head(gpa), GPA_HPT_MASK));
}

void debug_unmap_gpas(struct emp_mm *bvma, struct emp_gpa *head, u64 addr,
		bool *tlb_flush_force)
{
#ifdef CONFIG_EMP_VM
	u64 *spte, gfn;
	int i, j;

	if (!is_gpa_flags_set(head, GPA_EPT_MASK))
		return;

	gfn = (head->local_page->gpa_index << bvma_subblock_order(bvma))
			+ get_gfn_offset(head);
	spte = kvm_emp___get_spte(kvm_get_any_vcpu(bvma->ekvm.kvm), PG_LEVEL_4K, gfn);
	if (spte == NULL)
		return;

	for (i = 0; i < num_subblock_in_block(head); i++) {
		for (j = 0; j < gpa_subblock_size(head); j++) {
			if (!is_shadow_present_pte(*(spte + (i * num_subblock_in_block(head)) + j)))
				continue;

			printk(KERN_ERR "check unmap_gpas. "
					"spte: %p %llx\n", spte, *spte);
		}
	}
#endif /* CONFIG_EMP_VM */
}

void COMPILER_DEBUG debug_set_gpa_remote(struct emp_mm *bvma, struct emp_gpa *g)
{
	struct page *page;

	page = g->local_page->page;

	BUG_ON(emp_page_count_max(page, gpa_subblock_size(g)) != 1);
}

void debug_clear_and_map_pages(struct emp_gpa *head)
{
	struct emp_gpa *g;
	struct page *p;
	for_each_gpas(g, head) {
		int i;

		BUG_ON(g->local_page == NULL);
		p = g->local_page->page;
		for (i = 0; i < gpa_subblock_size(g); i++)
			BUG_ON(page_count(p + i) < 2);
	}
}

void debug___emp_page_fault_hva(struct emp_gpa *head) {
#ifdef CONFIG_EMP_VM
	BUG_ON(is_gpa_flags_set(head, GPA_HPT_MASK) &&
			is_gpa_flags_set(head, GPA_EPT_MASK));
#endif
	BUG_ON(is_gpa_flags_set(head, GPA_HPT_MASK) &&
			is_gpa_flags_set(head, GPA_PREFETCHED_CPF_MASK));
}

void debug___emp_page_fault_hva2(struct emp_gpa *head)
{
	int i;
	struct emp_gpa *g;
	struct page *p;

	for_each_gpas(g, head) {
		p = g->local_page->page;
		for (i = 0; i < gpa_subblock_size(g); i++)
			if (page_count(p + i) < 2)
				printk(KERN_ERR "WARN: %s page count is not matched. "
						"page_count(%016lx): %d < 2\n",
						__func__, (unsigned long)(p + i),
						page_count(p + i));
		}
}

void debug_pte_install(struct emp_mm *bvma, struct page *page, int page_compound_len)
{
	if (emp_page_count_min(page, page_compound_len) >= 1)
		return;
	printk("%s[%d] page count should be positive\n", __func__, __LINE__);
}

void debug_select_victims_al(struct list_head *to_lru_head, int to_lru_len)
{
	BUG_ON(to_lru_len && list_empty(to_lru_head));
}

void debug_wait_for_prefetch_subblocks(struct emp_gpa *g) {
	BUG_ON(is_gpa_flags_same(g, GPA_nPT_MASK, 0));
}

void debug_evict_block(struct emp_gpa *head)
{
	int i;
	struct emp_gpa *g;
	struct page *p;
	bool warn = false;

	for_each_gpas(g, head) {
		BUG_ON(g->local_page == NULL);
		p = g->local_page->page;
		for (i = 0; i < gpa_subblock_size(g); i++) {
			if (page_count(p + i) == 1)
				continue;

			warn = true;
			break;
		}
	}

	if (unlikely(warn)) {
		printk("%s target block %lx not free yet. ",
				__func__, head->local_page->gpa_index);
		for_each_gpas(g, head) {
			p = g->local_page->page;
			if (page_count(p) == 1)
				continue;

			printk("%s gpa %lx pc: %d", __func__,
					g->local_page->gpa_index,
					page_count(p));
		}
	}
}

void debug_update_inactive_list(struct emp_gpa *head, struct emp_gpa *g) {
	BUG_ON(head != g);
}

void debug_update_inactive_list2(struct emp_gpa *head)
{
	struct emp_gpa *g;
	int i;
	struct page *p;
	for_each_gpas(g, head) {
		p = g->local_page->page;
		for (i = 0; i < gpa_subblock_size(g); i++) {
			if (page_count(p + i) != 1)
				printk(KERN_ERR "WARN: %s page count is not matched. "
						"page_count(%016lx): %d != 1\n",
						__func__, (unsigned long)(p + i),
						page_count(p + i));
		}
	}
}

void debug_add_gpas_to_inactive(struct emp_gpa **gpas, int n_new) {
	BUG_ON(gpas && n_new == 0);
	BUG_ON(gpas == NULL && n_new != 0);
}

void debug_add_gpas_to_inactive2(struct emp_gpa *head, struct emp_gpa *g) {
	BUG_ON(head != g);
}

void debug_flush_direct_pages(struct emp_gpa *g) {
	BUG_ON(is_gpa_flags_set(g, GPA_PREFETCHED_MASK));
}

bool check_gpa_block_reclaimable(struct emp_mm *bvma, struct emp_gpa *gpa);
void debug_flush_direct_pages2(struct emp_mm *bvma, struct emp_gpa *head) {
	if (!check_gpa_block_reclaimable(bvma, head))
		printk("%s forced_reclaim: gfn %lx pfn %lx",
				__func__, head->local_page->gpa_index,
				page_to_pfn(head->local_page->page));
}

void debug_unregister_bvma(struct emp_mm *bvma)
{
	int i;
	extern struct emp_mm **emp_mm_arr;

	for (i = 0; i < EMP_MM_MAX; i++)
		BUG_ON(emp_mm_arr[i] == bvma);
}

void debug_add_gpas_to_active_list(struct emp_gpa **gpas, int n_new) {
	BUG_ON(gpas && n_new == 0);
	BUG_ON(gpas == NULL && n_new != 0);
}

void debug_add_list_count(struct slru *target_list, int count) {
	target_list->list_count += count;
}

void debug_sub_list_count(struct slru *target_list, int count) {
	target_list->list_count -= count;
}

void debug_free_local_page(struct local_page *local_page) {
	/* check that the local page was popped from the lru list */
	BUG_ON(!list_empty(&local_page->lru_list) &&
		!(local_page->lru_list.prev == LIST_POISON2
			&& local_page->lru_list.next == LIST_POISON1));
	BUG_ON(!EMP_LP_PMDS_EMPTY(&local_page->pmds));
	BUG_ON(is_local_page_on_list(local_page));
}

void debug_unmap_ptes(struct emp_mm *emm, struct emp_gpa *heads, unsigned long size) {
	struct emp_gpa *g, *heads_end;
	bool warned;

	heads_end = heads + (size >> (bvma_subblock_order(emm) + PAGE_SHIFT));

	for (g = heads; g < heads_end; g++) {
		warned = false;
		if (page_mapcount(g->local_page->page)) {
			printk(KERN_ERR "WARN: %s page mapcount is not matched. "
					"gpa: %016lx idx: %lx page_mapcount(%016lx): %d != 0\n",
					__func__, (unsigned long) g,
					g->local_page->gpa_index,
					(unsigned long) g->local_page->page,
					page_mapcount(g->local_page->page));
			warned = true;
		}

		if (page_count(g->local_page->page) != 1) {
			printk(KERN_ERR "WARN: %s page count is not matched. "
					"gpa: %016lx idx: %lx page_count(%016lx): %d != 1\n",
					__func__, (unsigned long) g,
					g->local_page->gpa_index,
					(unsigned long) g->local_page->page,
					page_count(g->local_page->page));
			warned = true;
		}
	}

#ifdef CONFIG_EMP_DEBUG_SHOW_GPA_STATE
	if (warned && heads->local_page->vmr_id >= 0
			&& heads->local_page->vmr_id < EMP_VMRS_MAX
			&& emm->vmrs[heads->local_page->vmr_id] != NULL) {
		struct emp_vmr *vmr = emm->vmrs[heads->local_page->vmr_id];
		unsigned long idx = heads->local_page->gpa_index;
		bool head = true;
		emp_debug_bulk_msg_lock();
		for_each_gpas(g, heads) {
			__debug_show_gpa_state(vmr, g, idx, head);
			idx++;
			head = false;
		}
		emp_debug_bulk_msg_unlock();
	} else if (warned) {
		printk("WARN: %s gpa: %016lx idx: %lx is warned but cannot find vmr. vmr_id: %d\n",
				__func__, (unsigned long) heads, heads->local_page->gpa_index,
				heads->local_page->vmr_id);
	}
#endif
}

void debug_alloc_remote_page(struct emp_mm *emm, struct emp_gpa *head)
{
	bool is_free = is_gpa_remote_page_free(head);
	if (!is_free && !emm->config.remote_reuse) {
		printk_ratelimited(KERN_ERR "%s: gpa: %016lx remote_page(%016llx)"
					    " is already allocated.\n",
					    __func__, (unsigned long) head,
					    get_gpa_remote_page_val(head));
	}
}

void debug_alloc_remote_page2(struct emp_mm *emm, struct emp_gpa *head)
{
	struct emp_gpa *gpa;
	struct remote_page *remote_page;
	struct memreg *m;
	for_each_gpas(gpa, head) {
		if (is_gpa_remote_page_free(gpa))
			continue;

		remote_page = &gpa->remote_page;
		m = emm->mrs.memregs[get_remote_page_mrid(remote_page)];
		if (m->size <= get_remote_page_offset(remote_page)) {
			printk(KERN_ERR "FIXIT: out of range on remote storage."
					" remote_size: 0x%llx remote_page: %d:0x%lx\n",
					m->size, (int) get_remote_page_mrid(remote_page),
					(unsigned long) get_remote_page_offset(remote_page));
			BUG();
		}
	}
}

void debug_free_gpa_dir_region(struct emp_gpa *head, int desc_order) {
	int i, end;
	int refcnt0;
	if (desc_order == 0)
		return;
	refcnt0 = atomic_read(&head->refcnt);
	end = 1 << desc_order;
	for (i = 1; i < end; i++)
		BUG_ON(atomic_read(&(head + i)->refcnt) != refcnt0);
}

void debug_emp_lp_count_pmd(struct local_page *lp)
{
	int count;
	struct mapped_pmd *p = &lp->pmds;

	if (EMP_LP_PMDS_EMPTY(p))
		count = 0;
	else if (EMP_LP_PMDS_SINGLE(p))
		count = 1;
	else {
		count = 0;
		do {
			count++;
			p = p->next;
		} while (p != &lp->pmds);
	}

	BUG_ON(lp->num_pmds != count);
}
EXPORT_SYMBOL(debug_emp_lp_count_pmd);

void debug_reclaim_exit_active(struct emp_mm *emm, struct slru *active)
{
	struct emp_list *list;
#ifdef CONFIG_EMP_VM
	if (active->mru_bufs) {
		int i;
		for (i = 0; i < active->abuf_len; i++) {
			list = active->mru_bufs + i;
			emp_list_lock(list);
			BUG_ON(!emp_list_empty(list));
			BUG_ON(emp_list_len(list) != 0);
			emp_list_unlock(list);
		}
	}
#endif /* CONFIG_EMP_VM */

	if (active->host_mru) {
		int cpu;
		for_each_possible_cpu(cpu) {
			list = emp_pc_ptr(active->host_mru, cpu);
			emp_list_lock(list);
			BUG_ON(!emp_list_empty(list));
			BUG_ON(emp_list_len(list) != 0);
			emp_list_unlock(list);
		}
	}

#ifdef CONFIG_EMP_VM
	if (active->lru_bufs) {
		int i;
		for (i = 0; i < active->abuf_len; i++) {
			list = active->lru_bufs + i;
			emp_list_lock(list);
			BUG_ON(!emp_list_empty(list));
			BUG_ON(emp_list_len(list) != 0);
			emp_list_unlock(list);
		}
	}
#endif /* CONFIG_EMP_VM */

	if (active->host_lru) {
		int cpu;
		for_each_possible_cpu(cpu) {
			list = emp_pc_ptr(active->host_lru, cpu);
			emp_list_lock(list);
			BUG_ON(!emp_list_empty(list));
			BUG_ON(emp_list_len(list) != 0);
			emp_list_unlock(list);
		}
	}

	BUG_ON(!emp_list_empty(&active->list));
	BUG_ON(emp_list_len(&active->list) > 0);
	BUG_ON(atomic_read(&active->page_len) != 0);
}

void debug_reclaim_exit_inactive(struct emp_mm *emm, struct slru *inactive)
{
	struct emp_list *list;
#ifdef CONFIG_EMP_VM
	if (inactive->sync_bufs) {
		int i;
		for (i = 0; i < inactive->abuf_len; i++) {
			list = inactive->sync_bufs + i;
			emp_list_lock(list);
			BUG_ON(!emp_list_empty(list));
			BUG_ON(emp_list_len(list) != 0);
			emp_list_unlock(list);
		}
	}
#endif /* CONFIG_EMP_VM */

	if (inactive->host_lru) {
		int cpu;
		for_each_possible_cpu(cpu) {
			list = emp_pc_ptr(inactive->host_lru, cpu);
			emp_list_lock(list);
			BUG_ON(!emp_list_empty(list));
			BUG_ON(emp_list_len(list) != 0);
			emp_list_unlock(list);
		}
	}

#ifdef CONFIG_EMP_VM
	/* Check writeback list here, since it is a part of inactive list */
	if (emm->vcpus) {
		int id, len = EMP_KVM_VCPU_LEN(emm);
		struct vcpu_var *v;
		for (id = 0; id < len; id++) {
			v = &emm->vcpus[id];
			spin_lock(&v->wb_request_lock);
			BUG_ON(!list_empty(&v->wb_request_list));
			BUG_ON(v->wb_request_size > 0);
			spin_unlock(&v->wb_request_lock);
		}
	}
#endif

	if (emm->pcpus) {
		int id;
		struct vcpu_var *v;
		for_each_possible_cpu(id) {
			v = per_cpu_ptr(emm->pcpus, id);
			spin_lock(&v->wb_request_lock);
			BUG_ON(!list_empty(&v->wb_request_list));
			BUG_ON(v->wb_request_size > 0);
			spin_unlock(&v->wb_request_lock);
		}
	}

	BUG_ON(!emp_list_empty(&inactive->list));
	BUG_ON(emp_list_len(&inactive->list) > 0);
	BUG_ON(atomic_read(&inactive->page_len) != 0);
}

void debug_push_local_free_page(struct page *page) {
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
	int cnt = page_count(page);
	WARN(cnt != 1,
		"reference count of page is not 1 at push_local_free_page(). page_count: %d\n",
		cnt);
#endif
}

void debug_pop_local_free_page(struct page *page) {
	int cnt = page ? page_count(page) : INT_MIN;
	WARN(cnt != 1,
		"reference count of page is not 1 at pop_local_free_page(). page_count: %d\n",
		cnt);
}

void debug_alloc_exit(struct emp_mm *emm) {
	struct emp_list *free_page_list = &emm->ftm.free_page_list;
	WARN_ON(emp_list_len(free_page_list) != 0);
	WARN_ON(atomic_read(&emm->ftm.alloc_pages_len) != 0);
}

#ifdef CONFIG_EMP_DEBUG_LRU_LIST
void __debug_add_inactive_list_page_len(struct emp_mm *emm, struct emp_gpa *gpa,
					char *file, int line)
{
	BUG_ON(emp_get_block_head(gpa) != gpa);
	if (gpa->contrib_inactive_len != 0) {
		printk(KERN_ERR "%s: ERROR: contrib != 0. contrib: %d "
				"block_size: %ld at %s:%d "
				"last_contrib: %d at %s:%d\n",
				__func__, gpa->contrib_inactive_len,
				gpa_block_size(gpa), file, line,
				gpa->contrib_last_val,
				gpa->contrib_last_file ? gpa->contrib_last_file
						       : "(null)",
				gpa->contrib_last_line);
		//BUG();
	}
	gpa->contrib_inactive_len += gpa_block_size(gpa);
	gpa->contrib_last_file = file;
	gpa->contrib_last_line = line;
	gpa->contrib_last_val = gpa_block_size(gpa);
}
EXPORT_SYMBOL(__debug_add_inactive_list_page_len);

void __debug_sub_inactive_list_page_len(struct emp_mm *emm, struct emp_gpa *gpa,
					char *file, int line)
{
	BUG_ON(emp_get_block_head(gpa) != gpa);
	if (gpa->contrib_inactive_len != gpa_block_size(gpa)) {
		printk(KERN_ERR "%s: ERROR: contrib != block_size. contrib: %d "
				"block_size: %ld at %s:%d "
				"last_contrib: %d at %s:%d\n",
				__func__, gpa->contrib_inactive_len,
				gpa_block_size(gpa), file, line,
				gpa->contrib_last_val,
				gpa->contrib_last_file ? gpa->contrib_last_file
						       : "(null)",
				gpa->contrib_last_line);
		//BUG();
	}
	gpa->contrib_inactive_len -= gpa_block_size(gpa);
	gpa->contrib_last_file = file;
	gpa->contrib_last_line = line;
	gpa->contrib_last_val = -gpa_block_size(gpa);
}
EXPORT_SYMBOL(__debug_sub_inactive_list_page_len);
#endif

#endif
