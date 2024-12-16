#ifndef __DEBUG_GPA_H__
#define __DEBUG_GPA_H__

#include "config.h"

struct emp_gpa;
struct emp_vmr;

#ifdef CONFIG_EMP_DEBUG_PROGRESS

#define MAX_PROGRESS_LEN (32)
struct progress_info {
	int next;
	int cpu;
	struct task_struct *task;
	struct progress_history {
		char *file;
		int line;
		union {
			unsigned long long data;
			void *ptr; /* To show hexadecimal value in gdb */
		};
	} history[MAX_PROGRESS_LEN];
};

#define ____set_progress_history(history, __file, __line, __data) do { \
	(history)->file = (__file); \
	(history)->line = (__line); \
	(history)->data = (__data); \
} while (0)

#define ____set_progress_at_start(info) do { \
	(info)->cpu = smp_processor_id(); \
	(info)->task = current; \
} while (0)

#define __init_progress_info(info) do { \
	int ____i; \
	(info)->next = 0; \
	(info)->cpu = -1; \
	(info)->task = NULL; \
	for (____i = 0; ____i < MAX_PROGRESS_LEN; ____i++) { \
		____set_progress_history(&((info)->history[____i]), NULL, -1, 0); \
	} \
} while (0)

#define __debug_progress(info, data) do { \
	____set_progress_history(&(info)->history[(info)->next], \
					__FILE__, __LINE__, \
					(unsigned long long) (data)); \
	(info)->next = ((info)->next + 1) % MAX_PROGRESS_LEN; \
} while (0)

#define __debug_progress_start(info, data) do { \
	____set_progress_at_start(info); \
	__debug_progress(info, data); \
} while (0)

#define debug_progress(p, data) do { if (p) __debug_progress(&(p)->progress, (data)); } while (0)
#define debug_progress_start(p, data) do { if (p) __debug_progress_start(&(p)->progress, (data)); } while (0)
#define init_progress_info(p) __init_progress_info(&((p)->progress))

// This header file can be included before than any other header files
// except for config.h. Therefore, we include some duplucated macros.
#define debug_gpa_num_subblock_in_block(g) (1)
#define debug_gpa_for_each_gpas(pos, head) \
	for (pos = (head); \
		pos < ((head) + (debug_gpa_num_subblock_in_block(head))); \
		pos++)

#define debug_progress_start_block(head, data) do { \
	struct emp_gpa *____g; \
	if (head) { \
		debug_gpa_for_each_gpas(____g, (head)) { \
			debug_progress_start(____g, (data)); \
		} \
	} \
} while (0)

#define debug_progress_block(head, data) do { \
	struct emp_gpa *____g; \
	if (head) { \
		debug_gpa_for_each_gpas(____g, (head)) { \
			debug_progress(____g, (data)); \
		} \
	} \
} while (0)

#else /* !CONFIG_EMP_DEBUG_PROGRESS */
#define debug_progress(p, data) do {} while (0)
#define debug_progress_start(p, data) do {} while (0)
#define init_progress_info(p) do {} while (0)
#define debug_progress_start_block(gpa, data) do {} while (0)
#define debug_progress_block(gpa, data) do {} while (0)
#endif /* !CONFIG_EMP_DEBUG_PROGRESS */

#ifdef CONFIG_EMP_DEBUG_GPADESC_ALLOC

struct gpadesc_alloc_at {
	char *file;
	int line;
};

void set_gpadesc_alloc_at(struct emp_vmr *vmr, unsigned long index, char *file, int line);
#endif

#ifdef CONFIG_EMP_DEBUG_GPA_REFCNT
#define DEBUG_GPA_REFCNT_SIZE (16)
enum debug_gpa_refcnt_type {
	DEBUG_GPA_REFCNT_INIT = 0,
	DEBUG_GPA_REFCNT_INC = 1,
	DEBUG_GPA_REFCNT_DEC = 2
};

struct debug_gpa_refcnt {
	struct {
		char *file;
		int line;
		enum debug_gpa_refcnt_type type;
		int vmr_id;
		int curr;
	} history[DEBUG_GPA_REFCNT_SIZE];
	int next;
};

#define debug_gpa_refcnt_init(gpa) do { \
	int ____i; \
	for (____i = 0; ____i < DEBUG_GPA_REFCNT_SIZE; ____i++) { \
		(gpa)->debug_gpa_refcnt.history[____i].file = NULL; \
		(gpa)->debug_gpa_refcnt.history[____i].line = -1; \
		(gpa)->debug_gpa_refcnt.history[____i].type = DEBUG_GPA_REFCNT_INIT; \
		(gpa)->debug_gpa_refcnt.history[____i].vmr_id = -1; \
	} \
	(gpa)->debug_gpa_refcnt.next = 0; \
} while (0)

#define debug_gpa_refcnt_inc_mark(gpa, __vmr_id) do { \
	int ____i = (gpa)->debug_gpa_refcnt.next; \
	(gpa)->debug_gpa_refcnt.history[____i].file = __FILE__; \
	(gpa)->debug_gpa_refcnt.history[____i].line = __LINE__; \
	(gpa)->debug_gpa_refcnt.history[____i].type = DEBUG_GPA_REFCNT_INC; \
	(gpa)->debug_gpa_refcnt.history[____i].vmr_id = (__vmr_id); \
	(gpa)->debug_gpa_refcnt.history[____i].curr = atomic_read(&(gpa)->refcnt); \
	(gpa)->debug_gpa_refcnt.next = (____i + 1) % DEBUG_GPA_REFCNT_SIZE; \
} while (0)

#define debug_gpa_refcnt_dec_mark(gpa, __vmr_id) do { \
	int ____i = (gpa)->debug_gpa_refcnt.next; \
	(gpa)->debug_gpa_refcnt.history[____i].file = __FILE__; \
	(gpa)->debug_gpa_refcnt.history[____i].line = __LINE__; \
	(gpa)->debug_gpa_refcnt.history[____i].type = DEBUG_GPA_REFCNT_DEC; \
	(gpa)->debug_gpa_refcnt.history[____i].vmr_id = (__vmr_id); \
	(gpa)->debug_gpa_refcnt.history[____i].curr = atomic_read(&(gpa)->refcnt); \
	(gpa)->debug_gpa_refcnt.next = (____i + 1) % DEBUG_GPA_REFCNT_SIZE; \
} while (0)
#else
#define debug_gpa_refcnt_init(gpa) do {} while (0)
#define debug_gpa_refcnt_inc_mark(gpa, vmr_id) do {} while (0)
#define debug_gpa_refcnt_dec_mark(gpa, vmr_id) do {} while (0)
#endif

#ifdef CONFIG_EMP_DEBUG_GPA_STATE
#define debug_progress_flag(gpa, flag) do { \
	unsigned long long ____d = (((unsigned long long) (gpa)->flags) << 32) \
					| (flag); \
	debug_progress(gpa, ____d); \
} while (0)
#else
#define debug_progress_flag(gpa, flag) do {} while (0)
#endif

#endif /* __DEBUG_GPA_H__ */
