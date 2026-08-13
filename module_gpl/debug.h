#ifndef __DEBUG_H__
#define __DEBUG_H__

/* NOTE: on/off selective debugging features in config.h */
#include "config.h"
#include "gpa.h"
#include "vm.h"
#include "lru.h"
#include "debug_assert.h"

/******************** EMP DEBUG BULK MESSAGE LOCK ***********************/
void emp_debug_bulk_msg_lock(void);
void emp_debug_bulk_msg_unlock(void);

/******************** EMP DEBUG PAGEFAULT HISTORY ***********************/
#ifdef CONFIG_EMP_DEBUG_PF_HISTORY
/* struct emp_pf_data and struct emp_pf_history are defined in vcpu_var.h */
int emp_pf_history_init(struct vcpu_var *);
void emp_pf_history_beg(struct vcpu_var *, int, unsigned long);
#define __emp_pf_history_add(data, var, val) do { \
		(data)->var = (val); \
} while (0)
#define emp_pf_history_add(cpu, var, val) do { \
	if (likely((cpu)->pf_history)) { \
		long ____curr_idx = atomic64_read(&(cpu)->pf_history->curr_idx); \
		(cpu)->pf_history->data[____curr_idx].var = (val); \
	} \
} while (0)
void __emp_pf_history_end(struct vcpu_var *, const char *);
#define emp_pf_history_end(v) __emp_pf_history_end(v, __func__)
#else
#define emp_pf_history_init(v) (0)
#define emp_pf_history_beg(v, vmr_id, addr) do {} while (0)
#define emp_pf_history_add(v, var, val) do {} while (0)
#define emp_pf_history_end(v) do {} while (0)
#endif

/******************** EMP DEBUG RSS *************************************/
#ifdef CONFIG_EMP_DEBUG_RSS
#define DEBUG_RSS_BITMAP_U64LEN ((CONFIG_EMP_DEBUG_RSS_MAX_VMRS + (sizeof(u64)*8) - 1) \
								/ (sizeof(u64)*8))
void debug_update_rss_add(struct emp_vmr *vmr, long val, int ID, int by_emp,
			struct emp_gpa *gpa, int mode, char *file, int line);
void debug_update_rss_sub(struct emp_vmr *vmr, long val, int ID, int by_emp,
			struct emp_gpa *gpa, int mode, char *file, int line);
void debug_update_rss_init_local_page(struct local_page *lp);
void debug_update_rss_free_local_page(struct local_page *lp);
#else
#define debug_update_rss_init_local_page(lp) do {} while (0)
#define debug_update_rss_free_local_page(lp) do {} while (0)
#endif

/**************************** EMP DEBUG PAGE REF ************************/
#ifdef CONFIG_EMP_DEBUG_PAGE_REF
void __debug_page_ref_print_all(struct local_page *lp);
void debug_check_page_map_status(struct emp_vmr *vmr, struct emp_gpa *head,
			unsigned long head_idx, pmd_t *pmd, bool map_expected);
#else
#define __debug_page_ref_print_all(lp) do {} while (0)
#define debug_check_page_map_status(vmr, head, head_idx, pmd, map_expected) do {} while (0)
#endif

/**************************** EMP DEBUG GPA STATE ***********************/
#ifdef CONFIG_EMP_DEBUG_SHOW_GPA_STATE
void debug_show_gpa_state(struct emp_vmr *vmr, const char *func);
void __debug_show_gpa_state(struct emp_vmr *vmr, struct emp_gpa *gpa, unsigned long idx, bool head);
#else
#define debug_show_gpa_state(vmr, func) do {} while (0)
#define __debug_show_gpa_state_lock() do {} while (0)
#define __debug_show_gpa_state_unlock() do {} while (0)
#define __debug_show_gpa_state(vmr, gpa, idx, head) do {} while (0)
#endif

/**************************** EMP DEBUG ALLOC ***************************/
#ifdef CONFIG_EMP_DEBUG_ALLOC
void __debug_alloc(void *__addr, size_t size, const char *file, const int line);
void __debug_free(void *__addr, const char *file, const int line);
void __debug_cache_create(void *__addr, size_t size, const char *file, const int line);
size_t __debug_cache_get_size(void *__addr, const char *file, const int line);
void __debug_cache_destroy(void *__addr, const char *file, const int line);

#define emp_debug_alloc(addr, size) do { \
	static const char *file = __FILE__; \
	static const int line = __LINE__; \
	__debug_alloc(addr, size, file, line); \
} while (0)
#define emp_debug_free(addr) do { \
	static const char *file = __FILE__; \
	static const int line = __LINE__; \
	__debug_free(addr, file, line); \
} while (0)
void emp_debug_alloc_init(void);
void emp_debug_alloc_exit(void);
#define __emp_alloc(func, size, ...) ({\
	void *____ret = (func)(__VA_ARGS__); \
	emp_debug_alloc(____ret, size); \
	____ret; \
})

#define __emp_free(func, addr, ...) do {\
	emp_debug_free(addr); \
	(func)(__VA_ARGS__); \
} while (0)

#define __emp_cache_create(name, size, align, flags, ctor) ({\
	static const char *file = __FILE__; \
	static const int line = __LINE__; \
	struct kmem_cache *____ret = kmem_cache_create(name, size, align, flags, ctor); \
	__debug_cache_create(____ret, size, file, line); \
	____ret; \
})

#define __emp_cache_destroy(cachep) do {\
	static const char *file = __FILE__; \
	static const int line = __LINE__; \
	__debug_cache_destroy((void *) cachep, file, line); \
	kmem_cache_destroy(cachep); \
} while(0)

#define __emp_cache_alloc(cachep, flags) ({\
	static const char *file = __FILE__; \
	static const int line = __LINE__; \
	void *____ret = kmem_cache_alloc(cachep, flags); \
	size_t ____size = __debug_cache_get_size(cachep, file, line); \
	__debug_alloc(____ret, ____size, file, line); \
	____ret; \
})

#define __emp_cache_free(cachep, addr) do {\
	static const char *file = __FILE__; \
	static const int line = __LINE__; \
	__debug_free(addr, file, line); \
	kmem_cache_free(cachep, addr); \
} while(0)

#define __emp_alloc_percpu(type) ({\
	static const char *file = __FILE__; \
	static const int line = __LINE__; \
	void __percpu *____ret = alloc_percpu(type); \
	__debug_alloc(per_cpu_ptr(____ret, 0), sizeof(type), file, line); \
	____ret; \
})

#define __emp_free_percpu(ptr) do {\
	static const char *file = __FILE__; \
	static const int line = __LINE__; \
	__debug_free(per_cpu_ptr(ptr, 0), file, line); \
	free_percpu(ptr); \
} while (0)

#else /* !CONFIG_EMP_DEBUG_ALLOC */
#define emp_debug_alloc_init() do{} while(0)
#define emp_debug_alloc_exit() do{} while(0)
#define __emp_alloc(func, size, ...) (func)(__VA_ARGS__)
#define __emp_free(func, addr, ...) (func)(__VA_ARGS__)
#define __emp_cache_create(name, size, align, flags, ctor) kmem_cache_create(name, size, align, flags, ctor)
#define __emp_cache_destroy(cachep) kmem_cache_destroy(cachep)
#define __emp_cache_alloc(cachep, flags) kmem_cache_alloc(cachep, flags)
#define __emp_cache_free(cachep, addr) kmem_cache_free(cachep, addr)
#define __emp_alloc_percpu(type) alloc_percpu(type)
#define __emp_free_percpu(ptr) free_percpu(ptr)
#endif /* !CONFIG_EMP_DEBUG_ALLOC */

#define emp_kmalloc(size, flag) __emp_alloc(kmalloc, size, size, flag)
#define emp_kzalloc(size, flag) __emp_alloc(kzalloc, size, size, flag)
#define emp_vmalloc(size) __emp_alloc(vmalloc, size, size)
#define emp_vzalloc(size) __emp_alloc(vzalloc, size, size)
#define emp_kfree(addr) __emp_free(kfree, addr, addr)
#define emp_vfree(addr) __emp_free(vfree, addr, addr)

#define emp_kmalloc_node(size, flag, node) \
	__emp_alloc(kmalloc_node, size, size, flag, node)
#define emp_kfree_node(addr) __emp_free(kfree, addr, addr)

#define emp_alloc_pages_node(nid, gfp, page_order) __emp_alloc(alloc_pages_node, PAGE_SIZE << (page_order), nid, gfp, page_order)
#define emp_alloc_pages(gfp, page_order) __emp_alloc(alloc_pages, PAGE_SIZE << (page_order), gfp, page_order)
#define emp_alloc_page(gfp) emp_alloc_pages(gfp, 0)
#define emp_free_pages(page) do { \
	debug_BUG_ON(PageCompound(page) && !PageHead(page));\
	debug_BUG_ON(page_ref_count(page) != 1); \
	emp_put_page(page); \
} while (0)
#define emp_free_page(page) emp_free_pages(page)
#define emp_put_page(page) __emp_free(put_page, page, page)

#define emp_kmem_cache_create(name, size, align, flags, ctor) __emp_cache_create(name, size, align, flags, ctor)
#define emp_kmem_cache_destroy(cachep) __emp_cache_destroy(cachep)
#define emp_kmem_cache_alloc(cachep, flags) __emp_cache_alloc(cachep, flags)
#define emp_kmem_cache_free(cachep, addr) __emp_cache_free(cachep, addr)

#define emp_alloc_percpu(type) __emp_alloc_percpu(type)
#define emp_free_percpu(ptr) __emp_free_percpu(ptr)

/**************************** EMP DEBUG INTEGRITY ***************************/
struct work_request;
#ifdef CONFIG_EMP_DEBUG_INTEGRITY
int debug_check_tag(struct work_request *w);
int debug_check_tag_upto(struct work_request *w, void *upto);
void debug_generate_tag(struct emp_gpa *gpa);
#else
static inline int debug_check_tag(struct work_request *w) { return 0; }
static inline int debug_check_tag_upto(struct work_request *w, void *upto) { return 0; }
static inline void debug_generate_tag(struct emp_gpa *gpa) {}
#endif

/**************************** EMP DEBUG SHOW PROGRESS ***********************/
#ifdef CONFIG_EMP_DEBUG_SHOW_PROGRESS
#define dprintk_progress(...) printk(KERN_ERR __VA_ARGS__)
#else
#define dprintk_progress(...) do {} while (0)
#endif

/************************************************************************/

#ifdef CONFIG_EMP_DEBUG

#define debug_check_null_pointer(p) do { BUG_ON(p == NULL); } while(0)
#define debug_check_notnull_pointer(p) do { BUG_ON(p); } while(0)
#define debug_check_notequal(a, b) do {	BUG_ON(a != b); } while(0)
#define debug_check_lessthan(a, b) do {	BUG_ON(a < b); } while(0)
#define debug_check_notlocked(g) do { BUG_ON(!____emp_gpa_is_locked(g)); } while(0)
#define debug_check_head(page) do { BUG_ON(PageCompound(page) && !PageHead(page)); } while(0)

void debug_emp_unlock_block(struct emp_gpa *head);
void debug__handle_gpa_on_inactive_fault(struct emp_mm *emm, struct emp_gpa *);
void debug_fetch_block(struct emp_mm *emm, struct emp_gpa *g, int fip);
void debug_map_spte(struct page *p);
void debug_check_sptep(u64 *sptep);
void debug_emp_map_prefetch_sptes2(struct emp_gpa *g, struct emp_gpa *eg);
void debug_emp_page_fault_gpa(struct emp_gpa *head);
void debug_emp_page_fault_gpa2(struct emp_mm *emm, struct emp_gpa *);
void debug_emp_page_fault_gpa3(struct emp_gpa *h, gva_t gva, pgoff_t o);
void debug_handle_active_fault_handled(struct emp_vmr *, struct emp_gpa *);
void debug___mmu_set_spte(u64 pfn);
void debug_map_sptes_in_subblock(struct emp_gpa *sb_head, u64 *sptep);
void debug_emp_install_sptes(struct emp_mm *, struct emp_gpa *);
void debug_emp_install_sptes2(struct emp_mm *, struct emp_gpa *, struct emp_gpa *);
void debug__alloc_pages(struct page *, int);
void debug_emp_bdev_wait_rw(struct work_request *, int);
void debug_alloc_and_fetch_pages(struct emp_mm *, struct emp_gpa *,
		struct page *, int, int);
void debug_alloc_and_fetch_pages2(struct emp_vmr *, struct emp_gpa *);
void debug_unmap_gpas(struct emp_mm *, struct emp_gpa *, u64, bool *);
void debug_set_gpa_remote(struct emp_mm *, struct emp_gpa *);
void debug_clear_and_map_pages(struct emp_mm *emm, struct emp_gpa *);
void debug___emp_page_fault_hva(struct emp_gpa *head);
void debug___emp_page_fault_hva2(struct emp_mm *, struct emp_gpa *);
void debug_pte_install(struct page *);
void debug_select_victims_pl(struct emp_gpa **, int);
void debug_select_victims_al(struct list_head *, int);
void debug_check_eager_wbr(struct emp_gpa *head, struct emp_gpa *g);
void debug_do_eager_writeback(struct emp_gpa *head);
void debug_check_eager_wbr2(struct emp_mm *emm, struct emp_gpa *);
void debug_evict_block(struct emp_mm *, struct emp_gpa *);
void debug_update_inactive_list(struct emp_gpa *head, struct emp_gpa *g);
void debug_update_inactive_list2(struct emp_mm *, struct emp_gpa *);
void debug_add_gpas_to_inactive(struct emp_gpa **gpas, int n_new);
void debug_add_gpas_to_inactive2(struct emp_gpa *head, struct emp_gpa *g);
void debug_flush_direct_pages(struct emp_gpa *g);
void debug_unregister_bvma(struct emp_mm *);
void debug_add_gpas_to_active_list(struct emp_gpa **gpas, int n_new, int type);
void debug_add_list_count(struct slru *target_list, int count);
void debug_sub_list_count(struct slru *target_list, int count);
void debug_free_local_page(struct local_page *local_page);
void debug_unmap_ptes(struct emp_mm *emm, struct emp_gpa *heads, unsigned long size);
void debug_alloc_remote_page(struct emp_mm *emm, struct emp_gpa *head);
void debug_alloc_remote_page2(struct emp_mm *emm, struct emp_gpa *head);
void debug_free_gpa_dir_region(struct emp_gpa *head, int desc_order);
#ifdef CONFIG_EMP_USER
void debug_check_vmdesc_views(struct emp_vmdesc *desc);
#endif
void debug_emp_lp_count_pmd(struct local_page *lp);
void debug_reclaim_exit_proactive(struct emp_mm *emm, struct slru *proactive);
void debug_reclaim_exit_active(struct emp_mm *emm, struct slru *active);
void debug_reclaim_exit_inactive(struct emp_mm *emm, struct slru *inactive);
void debug_push_free_page_list(struct page *page);
void debug_pop_free_page_list_local(struct page *page);
void debug_alloc_exit(struct emp_mm *emm);
#else /* !CONFIG_EMP_DEBUG */
#define debug_check_null_pointer(p) do{}while(0)
#define debug_check_notnull_pointer(p) do{}while(0)
#define debug_check_notequal(a, b) do{}while(0)
#define debug_check_lessthan(a, b) do{}while(0)
#define debug_check_notlocked(g) do{}while(0)
#define debug_check_head(page) do{}while(0)
#define debug_emp_unlock_block(head) do{}while(0)
#define debug__handle_gpa_on_inactive_fault(emm, g) do{}while(0)
#define debug_fetch_block(emm, g, fip) do{}while(0)
#define debug_map_spte(p) do{}while(0)
#define debug_check_sptep(sptep) do{}while(0)
#define debug_emp_map_prefetch_sptes2(g, eg) do{}while(0)
#define debug_emp_page_fault_gpa(head) do{}while(0)
#define debug_emp_page_fault_gpa2(emm, head) do{}while(0)
#define debug_emp_page_fault_gpa3(h, v, o) do{}while(0)
#define debug_handle_active_fault_handled(vmr, head) do {} while (0)
#define debug___mmu_set_spte(pfn) do{}while(0)
#define debug_map_sptes_in_subblock(sb_head, sptep) do{}while(0)
#define debug_emp_install_sptes(emm, g) do{}while(0)
#define debug_emp_install_sptes2(emm, h, g) do{}while(0)
#define debug__alloc_pages(page, page_order) do{}while(0)
#define debug_emp_bdev_wait_rw(w, rw) do{}while(0)
#define debug_alloc_and_fetch_pages(bvma, gpa, free_page, page_order, avail_dma_order) do{}while(0)
#define debug_alloc_and_fetch_pages2(v, gpa) do{}while(0)
#define debug_unmap_gpas(bvma, head, addr, tlb_flush_force) do{}while(0)
#define debug_set_gpa_remote(bvma, g) do{}while(0)
#define debug_clear_and_map_pages(emm, head) do{}while(0)
#define debug___emp_page_fault_hva(head) do{}while(0)
#define debug___emp_page_fault_hva2(emm, head) do{}while(0)
#define debug_pte_install(p) do{}while(0)
#define debug_select_victims_pl(vs, n_vs) do {} while (0)
#define debug_select_victims_al(h, l) do{}while(0)
#define debug_check_eager_wbr(head, g) do {} while(0)
#define debug_check_eager_wbr2(emm, head) do {} while(0)
#define debug_do_eager_writeback(head) do {} while(0)
#define debug_evict_block(emm, head) do{}while(0)
#define debug_update_inactive_list(head, g) do{}while(0)
#define debug_update_inactive_list2(emm, head) do{}while(0)
#define debug_add_gpas_to_inactive(gpas, n_new) do{}while(0)
#define debug_add_gpas_to_inactive2(head, g) do{}while(0)
#define debug_flush_direct_pages(g) do{}while(0)
#define debug_unregister_bvma(bvma) do{}while(0)
#define debug_add_gpas_to_active_list(gpas, n_new, type) do{}while(0)
#define debug_add_list_count(target_list, count) do{}while(0)
#define debug_sub_list_count(target_list, count) do{}while(0)
#define debug_free_local_page(lp) do {} while(0)
#define debug_unmap_ptes(emm, heads, size) do {} while(0)
#define debug_alloc_remote_page(emm, head) do {} while(0)
#define debug_alloc_remote_page2(emm, head) do {} while (0)
#define debug_free_gpa_dir_region(head, desc_order) do {} while (0)
#define debug_check_vmdesc_views(desc) do {} while (0)
#define debug_emp_lp_count_pmd(lp) do {} while (0)
#define debug_reclaim_exit_proactive(emm, slru) do {} while (0)
#define debug_reclaim_exit_active(emm, active) do {} while (0)
#define debug_reclaim_exit_inactive(emm, inactive) do {} while (0)
#define debug_push_free_page_list(page) do {} while (0)
#define debug_pop_free_page_list_local(page) do {} while (0)
#define debug_alloc_exit(emm) do {} while (0)
#endif /* !CONFIG_EMP_DEBUG */

#ifdef CONFIG_EMP_DEBUG_LRU_LIST
void __debug_add_inactive_list_page_len(struct emp_mm *emm, struct emp_gpa *gpa, char *file, int line);
void __debug_sub_inactive_list_page_len(struct emp_mm *emm, struct emp_gpa *gpa, char *file, int line);
#define debug_add_inactive_list_page_len(emm, gpa) __debug_add_inactive_list_page_len(emm, gpa, __FILE__, __LINE__)
#define debug_sub_inactive_list_page_len(emm, gpa) __debug_sub_inactive_list_page_len(emm, gpa, __FILE__, __LINE__)
void debug_add_inflight_writeback_page_len(struct emp_mm *emm, struct emp_gpa *gpa);
void debug_sub_inflight_writeback_page_len(struct emp_mm *emm, struct emp_gpa *gpa);
void __debug_reduce_inactive_list_page_len(struct emp_mm *emm, struct emp_gpa **gpa_list, int gpa_list_len, char *file, int line);
#define debug_reduce_inactive_list_page_len(emm, gpa_list, gpa_list_len) __debug_reduce_inactive_list_page_len(emm, gpa_list, gpa_list_len, __FILE__, __LINE__)
#else
#define debug_add_inactive_list_page_len(emm, gpa) do {} while (0)
#define debug_sub_inactive_list_page_len(emm, gpa) do {} while (0)
#define debug_add_inflight_writeback_page_len(emm, gpa) do {} while (0)
#define debug_sub_inflight_writeback_page_len(emm, gpa) do {} while (0)
#define debug_reduce_inactive_list_page_len(emm, gpa_list, gpa_list_len) do {} while (0)
#endif

#ifdef CONFIG_EMP_ELASTIC_BLOCK
#ifdef CONFIG_EMP_DEBUG
void debug_els_update_gpa_flags(struct emp_gpa *g);
void debug_els_stretch_rep(struct emp_gpa *, int, u32);
void debug_els_stretch_rep2(struct emp_gpa *, struct emp_gpa *);
void debug_els_stretch_rep3(struct emp_gpa *, struct emp_gpa *, int);
void debug_els_stretch_rep4(struct emp_gpa *);
void debug_els_stretch_rep5(struct emp_mm *, struct emp_vmr *, struct emp_gpa *,
							struct emp_gpa *, int);
void debug___els_reduce(struct emp_gpa *buddy, struct emp_gpa *s);
void debug___els_tryreduce_complete(struct emp_gpa *s);
void debug___els_tryreduce_complete2(struct emp_gpa *g, struct emp_gpa *s);
void debug___els_stretch(struct emp_gpa *, int);
#else /* !CONFIG_EMP_DEBUG */
#define debug_els_update_gpa_flags(g) do {} while(0)
#define debug_els_stretch_rep(chead, cpu, flag) do {} while(0)
#define debug_els_stretch_rep2(new_primary, prev_linked) do {} while(0)
#define debug_els_stretch_rep3(new_primary, chead, cpu) do {} while(0)
#define debug_els_stretch_rep4(new_primary) do {} while(0)
#define debug_els_stretch_rep5(bvma, v, new_primary, chead, cpu) do {} while(0)
#define debug___els_reduce(buddy, s) do {} while(0)
#define debug___els_tryreduce_complete(s) do {} while(0)
#define debug___els_tryreduce_complete2(g, s) do {} while(0)
#define debug___els_stretch(g, next_order) do {} while(0)
#endif /* !CONFIG_EMP_DEBUG */
#endif /* CONFIG_EMP_ELASTIC_BLOCK */


#endif
