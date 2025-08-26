#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef EMP_DEBUG
#include <stdio.h>
#endif
#include <stdio.h>

#include "emp/libemp.h"
#include "emp/mmap_impl.h"

#define	UNIT	16
#define	OFF_MASK ((-0x2000ULL << (8*sizeof(syscall_arg_t)-1)) | (UNIT-1))

extern long __syscall_ret(unsigned long r);

static void dummy(void) { }
weak_alias(dummy, __vm_wait);

typedef long syscall_arg_t;

/* EMP */
extern int empfd;
#define LIBEMP_READY (empfd != -1)

void *emp_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	unsigned long ret;

#ifdef EMP_DEBUG
	//fprintf(stderr, "%s(): addr=0x%lx, length=0x%lx, prot=%d, flags=%d, fd=%d, offset=%zu\n", __func__, addr, length, prot, flags, fd, offset);
#endif
	if (offset & OFF_MASK) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (length >= PTRDIFF_MAX) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	if (flags & MAP_FIXED) {
		__vm_wait();
	}
#ifdef SYS_mmap2
	if (LIBEMP_READY && ((fd == -1) || (prot & MAP_ANONYMOUS))) {
		int emp_flags = flags & ~MAP_ANONYMOUS;
		ret = __syscall(SYS_mmap2, addr, length, prot, emp_flags, empfd, offset/UNIT);
	}
	else {
		ret = __syscall(SYS_mmap2, addr, length, prot, flags, fd, offset/UNIT);
	}
#else
	if (LIBEMP_READY && ((fd == -1) || (prot & MAP_ANONYMOUS))) {
		int emp_flags = flags & ~MAP_ANONYMOUS;

		ret = __syscall(SYS_mmap, addr, length, prot, emp_flags, empfd, offset);
//#ifdef EMP_DEBUG
		fprintf(stderr, "%s(1): addr=0x%lx, length=0x%lx, prot=%d, flags=%d, fd=%d, offset=%zu, ret=0x%lx\n", __func__, addr, length, prot, emp_flags, empfd, offset, ret);
//#endif
	}
	else {
		ret = __syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
#ifdef EMP_DEBUG
		fprintf(stderr, "%s(): addr=0x%lx, length=0x%lx, prot=%d, flags=%d, fd=%d, offset=%zu, ret=0x%lx\n", __func__, addr, length, prot, flags, fd, offset, ret);
#endif
	}
#endif
	/* Fixup incorrect EPERM from kernel. */
	if (ret == -EPERM && !addr && (flags&MAP_ANON) && !(flags&MAP_FIXED))
		ret = -ENOMEM;

	return (void *)__syscall_ret(ret);
}

//weak_alias(emp_mmap, mmap);

//weak_alias(mmap, mmap64);
