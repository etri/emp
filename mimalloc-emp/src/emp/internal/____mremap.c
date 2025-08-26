#ifdef EMP_DEBUG
#include <stdio.h>
#endif
#include <stdio.h>

#define _GNU_SOURCE
#include <unistd.h>
#include <linux/mman.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

#include "mmap_impl.h"
#include "libemp.h"
#include "emp_ioctl.h"
#include "____mremap.h"

/* EMP */
extern int empfd;
#define LIBEMP_READY (empfd != -1)

#define HPAGE_SIZE (2UL << 20)

static void dummy(void) { }
weak_alias(dummy, __vm_wait);

void *____mremap(void *old_addr, size_t old_len, size_t new_len, int flags, ...)
{
	va_list ap;
	void *new_addr = NULL;
	void *ret = NULL;
	int alignment;
	struct {
		void *old_addr;
		size_t old_len;
		size_t new_len;
		int flags;
		void *new_addr;
		void *ret;
	} mremap;

#ifdef EMP_DEBUG
	//fprintf(stderr, "%s(): old_addr=0x%lx, old_len=0x%lx, new_len=0x%lx, flags=%d, new_addr=0x%lx\n", __func__, old_addr, old_len, new_len, flags, new_addr);
#endif
	if(new_len == old_len)
		return old_addr;

	if (new_len >= PTRDIFF_MAX) {
		errno = ENOMEM;
		return MAP_FAILED;
	}

	if (flags & MREMAP_FIXED) {
		__vm_wait();
		va_start(ap, flags);
		new_addr = va_arg(ap, void *);
		va_end(ap);
	}

	if(empfd > 0) {
		mremap.old_addr = old_addr;
		mremap.old_len = old_len;
		mremap.new_len = new_len;
		mremap.flags = flags;
		mremap.new_addr = new_addr;
		mremap.ret = 0;

		if(old_len < new_len) { /* expand */
			/* get address that matches the offset of old_addr */
			if(ioctl(empfd, IOCTL_MREMAP, &mremap) == -1) {
				return (MAP_FAILED);
			}

			ret = (void *) syscall(SYS_mremap, old_addr, old_len, new_len, MREMAP_MAYMOVE | MREMAP_FIXED, (void *) mremap.ret);
#ifdef EMP_DEBUG
			fprintf(stderr, "%s(): old_addr=0x%lx, old_len=0x%lx, new_len=0x%lx, flags=%d, new_addr=0x%lx, ret=0x%lx\n", __func__, old_addr, old_len, new_len, flags, mremap.ret, ret);
#endif
			fprintf(stderr, "          %s(): old_addr=0x%lx, old_len=0x%lx, new_len=0x%lx, flags=%d, new_addr=0x%lx, ret=0x%lx\n", __func__, old_addr, old_len, new_len, flags, mremap.ret, ret);
			return ret;
		}
		else { 	/* shrink */
			ret = (void *)syscall(SYS_mremap, old_addr, old_len, new_len, flags, new_addr);
			/* adjust the gpas of emp */
#if 0
                        if(ret != MAP_FAILED) {
                                if(ioctl(empfd, IOCTL_MREMAP, &mremap) == -1) {
                                        ret = MAP_FAILED;
                                }
                        }
#endif
#ifdef EMP_DEBUG
			fprintf(stderr, "%s(): old_addr=0x%lx, old_len=0x%lx, new_len=0x%lx, flags=%d, new_addr=0x%lx, ret=0x%lx\n", __func__, old_addr, old_len, new_len, flags, new_addr, ret);
#endif
			return ret;
		}
	}
	else {
		return (void *)syscall(SYS_mremap, old_addr, old_len, new_len, flags, new_addr);
	}
}

