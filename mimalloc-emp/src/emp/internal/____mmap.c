#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>

#ifdef EMP_DEBUG
#include <stdio.h>
#endif

#include "____mmap.h"

#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((weak, alias(#old)))

#define __syscall       syscall

#define	UNIT	16
#define	OFF_MASK ((-0x2000ULL << (8*sizeof(syscall_arg_t)-1)) | (UNIT-1))

extern long __syscall_ret(unsigned long r);

static void dummy(void) { }
weak_alias(dummy, __vm_wait);

typedef long syscall_arg_t;

void *____mmap(void *start, size_t len, int prot, int flags, int fd, off_t off)
{
	long ret;

	if (off & OFF_MASK) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (len >= PTRDIFF_MAX) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	if (flags & MAP_FIXED) {
		__vm_wait();
	}
#ifdef SYS_mmap2
	ret = __syscall(SYS_mmap2, start, len, prot, flags, fd, off/UNIT);
#else
	ret = __syscall(SYS_mmap, start, len, prot, flags, fd, off);
#endif

#ifdef EMP_DEBUG
	fprintf(stderr, "          %s(): start=0x%lx, len=0x%lx, prot=%d, flags=%d, fd=%d, off=%zu, ret=%lx\n", __func__, start, len, prot, flags, fd, off, ret);
#endif

	if (fd > 0) {
		;//memset((void *) ret, 0, len);
	}
	/* Fixup incorrect EPERM from kernel. */
	if (ret == -EPERM && !start && (flags&MAP_ANON) && !(flags&MAP_FIXED))
		ret = -ENOMEM;

	return (void *)__syscall_ret(ret);
}
