#ifdef EMP_DEBUG
#include <stdio.h>
#endif

#include "mmap_impl.h"
#include "libemp.h"

int __mprotect(void *addr, size_t len, int prot)
{
	size_t start, end;

#ifdef EMP_DEBUG
	fprintf(stderr, "%s(): addr=0x%lx, len=0x%lx, prot=%d\n", __func__, addr, len, prot);
#endif

	start = (size_t)addr & -PAGE_SIZE;
	end = (size_t)((char *)addr + len + PAGE_SIZE-1) & -PAGE_SIZE;

	return syscall(SYS_mprotect, start, end-start, prot);
}

weak_alias(__mprotect, mprotect);

