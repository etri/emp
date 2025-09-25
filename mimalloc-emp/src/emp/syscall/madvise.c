#ifdef EMP_DEBUG
#include <stdio.h>
#endif

#include <errno.h>
#include <string.h>

#include "emp/syscall_impl.h"

/* EMP */
extern int empfd;
#define LIBEMP_READY (empfd != -1)

int emp_madvise(void *addr, size_t length, int advice)
{
#ifdef EMP_DEBUG
	fprintf(stderr, "%s(): addr=0x%lx, length=0x%lx, advice=%d\n", __func__, (unsigned long)addr, length, advice);
#endif

	if (empfd > 0) {
		if(advice == MADV_DONTNEED) {
			return 0;
		}
		if(advice == MADV_FREE) {
			return 0;
		}
        
		return 0;
	} else {
		return __syscall(SYS_madvise, addr, length, advice);
	}
}
