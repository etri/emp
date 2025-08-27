#ifndef __SYSCALL_IMPL_H__
#define __SYSCALL_IMPL_H__ 
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((weak, alias(#old)))

#define __syscall       syscall

void *emp_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int emp_mprotect(void *addr, size_t len, int prot);
int emp_madvise(void *addr, size_t length, int advice);
void *emp_mremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address);
int emp_munmap(void *addr, size_t length);

#endif /* __SYSCALL_IMPL_H__ */
