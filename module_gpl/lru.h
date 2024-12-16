#ifndef __LRU_H__
#define __LRU_H__

#include <linux/list.h>
#include <linux/spinlock.h>
#include "debug_assert.h"

struct emp_list {
	atomic_t         len;
	spinlock_t       lock;
	struct list_head head;
};

struct temp_list {
	struct list_head head;
	int len;
};

static inline int __list_num_entry(struct list_head *head)
{
	int len = 0;
	struct list_head *pos;
	list_for_each(pos, head)
			len++;
	return len;
}

static inline void init_emp_list(struct emp_list *list) {
	INIT_LIST_HEAD(&list->head);
	atomic_set(&list->len, 0);
	spin_lock_init(&list->lock);
}

static inline void emp_list_lock(struct emp_list *list)
{
	spin_lock(&list->lock);
}

static inline int emp_list_trylock(struct emp_list *list)
{
	return spin_trylock(&list->lock);
}

static inline void emp_list_unlock(struct emp_list *list)
{
	spin_unlock(&list->lock);
}

static inline int emp_list_len(struct emp_list *list)
{
	return atomic_read(&list->len);
}

static inline bool emp_list_empty(struct emp_list *list)
{
	return emp_list_len(list) == 0;
}

static inline struct list_head *emp_list_head(struct emp_list *list) {
	return &list->head;
}

#define emp_list_for_each(cur, emp_list) list_for_each(cur, emp_list_head(emp_list))
#define emp_list_for_each_safe(cur, n, emp_list) list_for_each_safe(cur, n, emp_list_head(emp_list))
#define temp_list_for_each_safe(cur, n, temp_list) list_for_each_safe(cur, n, temp_list_head(temp_list))

static inline struct list_head *emp_list_pop_head(struct emp_list *list)
{
	struct list_head *ret;
	debug_assert(spin_is_locked(&list->lock));
	if (list_empty(&list->head))
		return NULL;
	ret = list->head.next;
	list_del_init(ret);
	atomic_dec(&list->len);
	return ret;
}

static inline struct list_head *emp_list_get_head(struct emp_list *list)
{
	return list->head.next;
}

static inline void emp_list_add_tail(struct list_head *head, struct emp_list *list)
{
	list_add_tail(head, &list->head);
	atomic_inc(&list->len);
	debug_assert(atomic_read(&list->len) > 0);
}

static inline void emp_list_del(struct list_head *pos, struct emp_list *list)
{
	list_del(pos);
	atomic_dec(&list->len);
	debug_assert(atomic_read(&list->len) >= 0);
}

static inline void emp_list_del_init(struct list_head *pos, struct emp_list *list)
{
	list_del_init(pos);
	atomic_dec(&list->len);
	debug_assert(atomic_read(&list->len) >= 0);
}

static inline void emp_list_splice_tail(struct temp_list *add,
					struct emp_list *list)
{
	debug_assert(spin_is_locked(&list->lock));
	debug_assert(__list_num_entry(&add->head) == add->len);
	list_splice_tail(&add->head, &list->head);
	atomic_add(add->len, &list->len);
	add->len = 0;
}

static inline void emp_list_splice_tail_emp_list(struct emp_list *add,
					struct emp_list *list)
{
	int add_len;
	debug_assert(spin_is_locked(&list->lock));
	debug_assert(spin_is_locked(&add->lock));
	debug_assert(__list_num_entry(&add->head) == atomic_read(&add->len));
	list_splice_tail(&add->head, &list->head);
	add_len = atomic_xchg(&add->len, 0);
	atomic_add(add_len, &list->len);
}

static inline void emp_list_drain(struct emp_list *list,
				struct temp_list *drain)
{
	list_splice_tail_init(&list->head, &drain->head);
	drain->len = atomic_read(&list->len);
	atomic_set(&list->len, 0);
}

static inline void emp_list_cut_position(struct emp_list *list,
		struct list_head *pos, int count, struct temp_list *cut)
{
#ifdef CONFIG_EMP_DEBUG_LRU_LIST
	struct list_head *p = list->head.next;
	int cnt = 1;
	while (cnt < count) {
		p = p->next;
		cnt++;
	}
	BUG_ON(pos != p);
#endif
	list_cut_position(&cut->head, &list->head, pos);
	atomic_sub(count, &list->len);
	cut->len += count;
}

static inline void init_temp_list(struct temp_list *list) {
	INIT_LIST_HEAD(&list->head);
	list->len = 0;
}

static inline int temp_list_len(struct temp_list *list) {
	return list->len;
}

static inline struct list_head *temp_list_head(struct temp_list *list) {
	return &list->head;
}

static inline bool temp_list_empty(struct temp_list *list)
{
	return temp_list_len(list) == 0;
}

static inline void temp_list_add_tail(struct list_head *head,
					struct temp_list *list)
{
	list_add_tail(head, &list->head);
	list->len++;
	debug_assert(list->len > 0);
}

static inline void temp_list_del(struct list_head *pos, struct temp_list *list)
{
	list_del(pos);
	list->len--;
	debug_assert(list->len >= 0);
}

static inline void temp_list_del_init(struct list_head *pos, struct temp_list *list)
{
	list_del_init(pos);
	list->len--;
	debug_assert(list->len >= 0);
}

struct slru {
	atomic_t        page_len;  // include *ru_bufs.len(s)
	struct emp_list list;
#ifdef CONFIG_EMP_DEBUG
	unsigned int list_count;
#ifdef CONFIG_EMP_VM
	int abuf_len;
#endif
#endif

#ifdef CONFIG_EMP_VM
	union {
		struct {
			struct emp_list *mru_bufs;
			struct emp_list *lru_bufs;
		};
		struct emp_list     *sync_bufs;
	};
#endif
	struct {
		struct emp_list **host_lru;
		struct emp_list **host_mru;
	};
};

#ifdef CONFIG_EMP_VM
#define get_list_ptr_mru(emm, slru, cpu_id) \
	(IS_IOTHREAD_VCPU(cpu_id) ? emp_pc_ptr((slru)->host_mru, PCPU_ID(emm, cpu_id)) \
	                          : ((slru)->mru_bufs + (cpu_id)))
#define get_list_ptr_lru(emm, slru, cpu_id) \
	(IS_IOTHREAD_VCPU(cpu_id) ? emp_pc_ptr((slru)->host_lru, PCPU_ID(emm, cpu_id)) \
	                          : ((slru)->lru_bufs + (cpu_id)))
#define get_list_ptr_inactive(emm, slru, cpu_id) \
	(IS_IOTHREAD_VCPU(cpu_id) ? emp_pc_ptr((slru)->host_lru, PCPU_ID(emm, cpu_id)) \
	                          : &(slru)->sync_bufs[cpu_id])
#else /* !CONFIG_EMP_VM */
#define get_list_ptr_mru(emm, slru, cpu_id) \
	(emp_pc_ptr((slru)->host_mru, PCPU_ID(emm, cpu_id)))
#define get_list_ptr_lru(emm, slru, cpu_id) \
	(emp_pc_ptr((slru)->host_lru, PCPU_ID(emm, cpu_id)))
#define get_list_ptr_inactive(emm, slru, cpu_id) \
	(emp_pc_ptr((slru)->host_lru, PCPU_ID(emm, cpu_id)))
#endif /* !CONFIG_EMP_VM */
#define read_inactive_list_page_len(emm) \
		atomic_read(&(emm)->ftm.inactive_list.page_len)

#define __add_inactive_list_page_len(val, emm) atomic_add(val, &(emm)->ftm.inactive_list.page_len)
#define __sub_inactive_list_page_len(val, emm) atomic_sub(val, &(emm)->ftm.inactive_list.page_len)

#define add_inactive_list_page_len(emm, gpa) do { \
		debug_add_inactive_list_page_len(emm, gpa); \
		__add_inactive_list_page_len(gpa_block_size(gpa), emm); \
} while (0)

#define sub_inactive_list_page_len(emm, gpa) do { \
		debug_sub_inactive_list_page_len(emm, gpa); \
		__sub_inactive_list_page_len(gpa_block_size(gpa), emm); \
} while (0)
#endif /* __LRU_H__ */
