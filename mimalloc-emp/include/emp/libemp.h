#ifndef __LIBEMP_H__
#define __LIBEMP_H__

#define		EMP_PAGE_SIZE	(4096*512)
#define		PAGE_SIZE	(4096)	

void libemp_init(void);
int get_empfd(void);
void libemp_exit(void);

#endif
