#ifndef __LIBEMP_H__
#define __LIBEMP_H__

#define		EMP_PAGE_SIZE	(4096*512)
#define		PAGE_SIZE	(4096)	

void libemp_init(void);
int get_empfd(void);
void libemp_exit(void);

extern int verbose;
extern int emp_enabled;

#ifdef __cplusplus
extern "C" {
#endif
	int emp_save(void);
	void emp_restore(int);
#ifdef __cplusplus
}
#endif

#define print_verbose(...)  \
	        if (verbose) fprintf(stderr, __VA_ARGS__)

#define LIBEMP_READY (empfd != -1 && emp_enabled)

#endif
