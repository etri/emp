#ifdef EMP_DEBUG
#include <stdio.h>
#endif

#include "emp/libemp.h"
#include "emp/syscall_impl.h"

/* EMP */
extern int empfd;

int emp_mprotect(void *addr, size_t len, int prot)
{
	size_t start, end;

#ifdef EMP_DEBUG
	fprintf(stderr, "%s(): addr=0x%lx, len=0x%lx, prot=%d\n", __func__, (unsigned long)addr, len, prot);
#endif

	start = (size_t)addr & -PAGE_SIZE;
	end = (size_t)((char *)addr + len + PAGE_SIZE-1) & -PAGE_SIZE;

	return syscall(SYS_mprotect, start, end-start, prot);
}
