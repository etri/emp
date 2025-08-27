#ifdef EMP_DEBUG
#include <stdio.h>
#endif

#include "emp/syscall_impl.h"

static void dummy(void) { }
weak_alias(dummy, __vm_wait);

int emp_munmap(void *addr, size_t length)
{
#ifdef EMP_DEBUG
	fprintf(stderr, "%s(): addr=0x%lx, length=0x%lx\n", __func__, (unsigned long)addr, length);
#endif
	__vm_wait();
	return __syscall(SYS_munmap, addr, length);
}
