#ifdef EMP_DEBUG
#include <stdio.h>
#endif

#include "mmap_impl.h"

static void dummy(void) { }
weak_alias(dummy, __vm_wait);

int __munmap(void *start, size_t len)
{
#ifdef EMP_DEBUG
	fprintf(stderr, "%s(): start=0x%lx, len=0x%lx\n", __func__, start, len);
#endif
	__vm_wait();
	return __syscall(SYS_munmap, start, len);
}

weak_alias(__munmap, munmap);
