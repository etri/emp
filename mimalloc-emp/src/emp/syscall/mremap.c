#define _GNU_SOURCE

#ifdef EMP_DEBUG
#include <stdio.h>
#endif
#include <errno.h>
#include <linux/mman.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>

#include "emp/libemp.h"
#include "emp/syscall_impl.h"

/* EMP */
extern int empfd;

void *emp_mremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address)
{
	void *ret = NULL;

#ifdef EMP_DEBUG
	fprintf(stderr, "%s(): old_address=0x%lx, old_size=0x%lx, new_size=0x%lx, flags=%d, new_address=0x%lx\n", __func__, (unsigned long)old_address, old_size, new_size, flags, (unsigned long)new_address);
#endif
	if (new_size == old_size)
		return old_address;

	if (new_size >= PTRDIFF_MAX) {
		errno = ENOMEM;
		return MAP_FAILED;
	}

	if (empfd > 0) {
		ret = mmap(new_address, new_size, PROT_READ|PROT_WRITE, flags, -1, 0);
		if (ret == MAP_FAILED) {
			return ret;
		}

		memcpy(ret, old_address, old_size < new_size ? old_size : new_size);
		munmap(old_address, old_size);

		return ret;
	} else {
		return (void *)syscall(SYS_mremap, old_address, old_size, new_size, flags, new_address);
	}
}
