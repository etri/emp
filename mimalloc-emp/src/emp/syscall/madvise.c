#ifdef EMP_DEBUG
#include <stdio.h>
#endif

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include "emp/syscall_impl.h"
#include "emp/emp_ioctl.h"

/* EMP */
extern int empfd;

int emp_madvise(void *addr, size_t length, int advice)
{
	struct emp_madv_info madv_info;

#ifdef EMP_DEBUG
	fprintf(stderr, "%s(): addr=0x%lx, length=0x%lx, advice=%d\n", __func__, (unsigned long)addr, length, advice);
#endif

	if (empfd > 0) {
		madv_info.addr = (unsigned long) addr;
		madv_info.size = length;
		madv_info.advice = advice;

		if (ioctl(empfd, IOCTL_EMP_MADV, &madv_info) == 0)
			return 0;
        
		return 0;
	} else {
		return __syscall(SYS_madvise, addr, length, advice);
	}
}
