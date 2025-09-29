#ifndef _EMP_IOCTL_
#define _EMP_IOCTL_
#ifdef _DEFAULT_SOURCE_ // glibc
#include <sys/mman.h>
#else
#include <asm-generic/mman-common.h>
#endif

#define IOCTL_UNMAP_ALL     0x10000
#define IOCTL_SAVE_PAGE     0x10001
#define IOCTL_FETCH_PAGE    0x10002
#define IOCTL_REQ           0x10003
#define IOCTL_CONN_DONOR    0x10004
#define IOCTL_GET_LOWER_SIZE 0x10005
#define IOCTL_HINT_IOV_W    0x10006
#define IOCTL_HINT_IOV_R    0x10007
#define IOCTL_REG_KVM       0x10008
#define IOCTL_UNMAP         0x10009
#define IOCTL_REG_MEM_REGION 0x1000A
#define IOCTL_SET_DRAM      0x1000B
#define IOCTL_FINI_CONN	    0x1000D
#define IOCTL_EMP_MADV      0x1000E

// advice codes for madvise() on EMP space
// *** codes from posix ***
// #define MADV_EMP_NORMAL             MADV_NORMAL
// #define MADV_EMP_RANDOM             MADV_RANDOM
// #define MADV_EMP_SEQUENTIAL         MADV_SEQUENTIAL
#define MADV_EMP_WILLNEED              MADV_WILLNEED
// #define MADV_EMP_DONTNEED           MADV_DONTNEED
// *** codes from linux ***
// #define MADV_EMP_REMOVE             MADV_REMOVE
// #define MADV_EMP_DONTFORK           MADV_DONTFORK
// #define MADV_EMP_DOFORK             MADV_DOFORK
// #define MADV_EMP_HWPOISON           MADV_HWPOISON
// #define MADV_EMP_MERGEABLE          MADV_MERGEABLE
// #define MADV_EMP_UNMERGEABLE        MADV_UNMERGEABLE
// #define MADV_EMP_SOFT_OFFLINE       MADV_SOFT_OFFLINE
// #define MADV_EMP_HUGEPAGE           MADV_HUGEPAGE
// #define MADV_EMP_NOHUGEPAGE         MADV_NOHUGEPAGE
// #define MADV_EMP_DONTDUMP           MADV_DONTDUMP
// #define MADV_EMP_DODUMP             MADV_DODUMP
// #define MADV_EMP_FREE               MADV_FREE
// #define MADV_EMP_WIPEONFORK         MADV_WIPEONFORK
// #define MADV_EMP_KEEPONFORK         MADV_KEEPONFORK
// #define MADV_EMP_COLD               MADV_COLD
// #define MADV_EMP_PAGEOUT            MADV_PAGEOUT
// *** EMP only codes ***
#define MADV_EMP_START                 (0x10000000)
#define MADV_EMP_PIN                   (MADV_EMP_START)
#define MADV_EMP_UNPIN                 (MADV_EMP_START + 1)

#define IOCTL_EMP_PINNING	0x1000F

#ifndef __KERNEL__
#include <netinet/in.h>
#include <stdbool.h>
#endif

enum donor_dev_type {
	DONOR_DEV_DRAM,
	DONOR_DEV_NVME,
	DONOR_DEV_RDMA,
	DONOR_DEV_PMEM,
	DONOR_DEV_MEMDEV
};

#ifdef __USE_DONOR_DEV_TYPE_STR__
static const char *donor_dev_type_str[] = {
	"dram",
	"nvme",
	"rdma",
	"pmem",
	"memdev"
};
#endif

struct donor_info {
	union {
#ifdef __KERNEL__
		u32                addr; // rdma
 #else
		in_addr_t          addr; // rdma
 #endif
		void               *ptr; // memdev
		unsigned long long loc;  // others
	};
	size_t        size;
	union {
		int           port; // rdma
		unsigned int  base; // nvme, pmem
		unsigned int  node; // memdev
	};

	int           dev_type;
	int           path_len;
	char          path[0];
};

struct emp_madv_info {
	unsigned long addr;
	unsigned long size;
	int advice;
};
#endif /* _EMP_IOCTL_ */
