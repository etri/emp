#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#define __USE_GNU
#include <dlfcn.h>

#include <emp_ioctl.h>
#include <libemp.h>
#include <libemp_cuda.h>
#include <env.h>

#define MADV_EMP_PINNING 30  //parameters of MADV_* is in linux [0,24]

static int empfd = -1;

int verbose = 0;
int emp_enabled = 0;

static void *(*real_mmap)(void *, size_t , int, int, int, off_t) = NULL;
static int (*real_madvise)(void *, size_t, int) = NULL;

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	print_verbose("empfd %d addr %lx length %lx prot %x "
			"flags %x fd %x offset %lx\n",
			empfd, (unsigned long) addr, length, prot,
			flags, fd, offset);

	if (LIBEMP_READY && ((fd == -1) || (prot & MAP_ANONYMOUS))) {
		int emp_flags = flags & ~MAP_ANONYMOUS;
		print_verbose("libemp.so: mmap for emp\n");
		/*return real_mmap(addr, length, prot, MAP_SHARED, empfd, offset);*/
		return real_mmap(addr, length, prot, emp_flags, empfd, offset);
	}

	print_verbose("libemp.so: mmap for stock memory allocator\n");
	return real_mmap(addr, length, prot, flags, fd, offset);
}

int madvise(void *addr, size_t length, int advise) {
	struct emp_prefetch pf_info;
	print_verbose("[libemp - madvise] empfd: %d addr: %lx length: %d %x advise: %d\n",
			empfd, (unsigned long) addr, length, length, advise);

	if (!LIBEMP_READY)
		goto fallback;

	switch (advise) {
	case MADV_NORMAL:
	case MADV_RANDOM:
	case MADV_SEQUENTIAL:
			break;
	case MADV_WILLNEED:
			pf_info.addr = (unsigned long) addr;
			pf_info.size = length;
			if (ioctl(empfd, IOCTL_EMP_PREFETCH, &pf_info) == 0)
				return 0;
			break;
	case MADV_DONTNEED:
			break;
	case MADV_EMP_PINNING:
			pf_info.addr = (unsigned long) addr;
			pf_info.size = length;
			if (ioctl(empfd, IOCTL_EMP_PINNING, &pf_info) == 0)
				return 0;
			break;
	default:
			break;
	}

fallback:
	return real_madvise(addr, length, advise);
}

#define GET_NEXT_SYMBOL(ptr, name) ({ \
	(ptr) = dlsym(RTLD_NEXT, name); \
	if (!(ptr)) { \
		const char *errmsg = dlerror(); \
		print_verbose("libemp.so: unable to intercept %s\n", #name); \
		if (errmsg) \
			fprintf(stderr, "errmsg: %s\n", errmsg); \
	} \
	(ptr) ? 0 : -1; \
})

static void mem_alloc_init(void)
{
	if (GET_NEXT_SYMBOL(real_mmap, "mmap"))
		exit(1);

	if (GET_NEXT_SYMBOL(real_madvise, "madvise"))
		exit(1);

	print_verbose("libemp.so: successfully intercept mmap and madvise\n");
}

void emp_disable(void)
{
	print_verbose("emp is disabled\n");
	emp_enabled = 0;
}

void emp_enable(void)
{
	print_verbose("emp is enabled\n");
	emp_enabled = 1;
}

int emp_save(void)
{
	int emp_status = emp_enabled;
	emp_disable();
	return emp_status;
}

void emp_restore(int emp_status)
{
	if (emp_status)
		emp_enable();
	else
		emp_disable();
}

void __attribute__((constructor)) libemp_init() 
{
	char empfd_str[16];
	int ret;
	char *param_emp_path, *param_emp_verbose;

	param_emp_path = getenv("EMP_MEM_PATH");
	if (!param_emp_path) {
		fprintf(stderr, "libemp.so: [ERROR] no emp_path parameters\n");
		exit(-1);
	}

	param_emp_verbose = getenv("EMP_VERBOSE");
	if (param_emp_verbose)
		verbose = atoi(param_emp_verbose);

	mem_alloc_init();
	emp_cuda_runtime_init();

	if (param_emp_path[0] == '!') {
		// empfd was opened at parent process
		empfd = atoi(param_emp_path + 1);
		if (fdopen(empfd, "rw") == NULL)  {
			fprintf(stderr, "libemp.so: [ERROR] Failed to inherit emp module: %s\n", strerror(errno));
			exit(-1);
		}

		print_verbose("libemp.so: [INFO] succeed to inherit emp module. emp_fd: %d\n", empfd);

		emp_enable();
		return;
	}

	empfd = open("/dev/emp", O_RDWR);
	if (empfd < 0) {
		fprintf(stderr, "libemp.so: [ERROR] Failed to open emp module: %s\n", strerror(errno));
		exit(-1);
	}

	ret = emp_setup_media(empfd, param_emp_path);
	if (ret < 0) {
		fprintf(stderr, "libemp.so: [ERROR] Invalid arguments: %s\n", param_emp_path);
		close(empfd);
		exit(-1);
	}

	print_verbose("libemp.so: [INFO] succeed to load emp module. emp_fd: %d\n", empfd);

	snprintf(empfd_str, 16, "!%d", empfd);
	setenv("EMP_MEM_PATH", empfd_str, 1);

	emp_enable();
}

void __attribute__((destructor)) libemp_exit()
{
	close(empfd);
}
