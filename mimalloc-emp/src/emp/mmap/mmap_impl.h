#ifndef __MMAP_IMPL_H__
#define __MMAP_IMPL_H__

#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((weak, alias(#old)))

#define __syscall       syscall

#endif /* __MMAP_IMPL_H__ */
