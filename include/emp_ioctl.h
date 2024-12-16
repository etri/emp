#ifndef _EMP_IOCTL_
#define _EMP_IOCTL_

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

#endif /* _EMP_IOCTL_ */
