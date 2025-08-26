#ifdef EMP_DEBUG
#include <stdio.h>
#endif

#include <errno.h>
#include <string.h>

#include "mmap_impl.h"

int __madvise(void *addr, size_t len, int advice)
{
#ifdef EMP_DEBUG
	fprintf(stderr, "%s(): addr=0x%lx, len=0x%lx, advice=%d\n", __func__, addr, len, advice);
#endif

#if 0
	return __syscall(SYS_madvise, addr, len, advice);

#else
	if(advice == MADV_DONTNEED) {
		return 0;
	}
	if(advice == MADV_FREE) {
		return 0;
	}

	return 0;
#endif
}

weak_alias(__madvise, madvise);
