#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#define __USE_DONOR_DEV_TYPE_STR__
#include "emp/emp_ioctl.h"

/**
 * emp_get_donor_dev_type - get emp donor type: dram ? nvme ? pmem ? remote mememory
 * @param path emp memory path
 *
 * @retval DONOR_DEV_DRAM: dram
 * @retval DONOR_DEV_NVME: nvme, pmem
 * @retval DONOR_DEV_RDMA: remote memory
 */
static enum donor_dev_type emp_get_donor_dev_type(char *path)
{
	if (strncmp(path, "dram", 4) == 0 ||
			strncmp(path, "DRAM", 4) == 0)
		return DONOR_DEV_DRAM;

	if (strstr(path, "/dev/nvme") == path)
		return DONOR_DEV_NVME;

	/* pmem is supported as a nvme device */
	if (strstr(path, "/dev/pmem") == path)
		return DONOR_DEV_NVME;

	return DONOR_DEV_RDMA;
}

/**
 * emp_setup_media - resolve emp path and setup donor
 * @param fd emp fd
 * @pram path emp memory path
 *
 * @retval -EINVAL: Error
 * @retval 0: Success
 */ 
int emp_setup_media(int fd, const char *path)
{
	struct donor_info donor;
	int ret;
	char *token, *subtoken, *str, *c, term;
	char *saveptr1, *saveptr2 = NULL;
	char path_str[PATH_MAX];
	int path_len;
	enum donor_dev_type dev_type;
	bool is_rdma = false;

	if (path == NULL) {
		return -EINVAL;
	}

	memset(&donor, 0, sizeof(struct donor_info));
	term = '\0';
	strcpy(path_str, (char *) path);
	str = (char *)path_str;
	for (c = str; *c != term; c++) {
		switch (*c) {
		case '\"':
		case '\'':
			term = *c;
			str = c+1;
			break;
		}
	}
	if (*c == term)
		*c = '\0';

#ifdef EMP_DEBUG
	fprintf(stderr, "emp memory: %s\n", str);
#endif
	while ((token = strtok_r(str, "|", &saveptr1))) {
		subtoken = strtok_r(token, ":", &saveptr2);
		path_len = strlen(subtoken) + 1;

		donor.path_len = path_len;
		strncpy(donor.path, subtoken, path_len);
#ifdef EMP_DEBUG
		fprintf(stderr, "donor.path: %s\n", donor.path);
#endif

		dev_type = emp_get_donor_dev_type(donor.path);
#ifdef EMP_DEBUG
		fprintf(stderr, "memory_node: type: %s path: %s", 
				donor_dev_type_str[dev_type],
				dev_type != DONOR_DEV_DRAM ? donor.path : "DRAM");
#endif
		donor.dev_type = dev_type;

		subtoken = strtok_r(NULL, ":", &saveptr2);
#ifdef EMP_DEBUG
		fprintf(stderr, ", size: %s", subtoken);
#endif
		donor.size = atol(subtoken);

		subtoken = strtok_r(NULL, ":", &saveptr2);
		switch (dev_type) {
		case DONOR_DEV_RDMA:
			is_rdma = true;
		case DONOR_DEV_NVME:
		case DONOR_DEV_PMEM:
			if (!subtoken) {
				ret = -EINVAL;
				break;
			}

			donor.addr = dev_type==DONOR_DEV_RDMA?
				inet_addr(donor.path): 0;
			donor.port = atoi(subtoken);

			subtoken = strtok_r(NULL, ":", &saveptr2);
			ret = ioctl(fd, IOCTL_CONN_DONOR, &donor);

			if(ret == -1) fprintf(stderr, "err: ioctl: RDMA\n");
#ifdef EMP_DEBUG
			fprintf(stderr, ", %s: %d\n",
					dev_type==DONOR_DEV_RDMA?"port":"offset",
					donor.port);
#endif
			break;
		case DONOR_DEV_DRAM:
			ret = ioctl(fd, IOCTL_SET_DRAM, &donor);
			if(ret == -1) fprintf(stderr, "err: ioctl: RDMA\n");
#ifdef EMP_DEBUG
			fprintf(stderr, "\n");
#endif
			break;
		}

		if (ret)
			break;
		str = NULL;
	}

	if (is_rdma) {
		if (ioctl(fd, IOCTL_FINI_CONN, 0))
			return -EINVAL;
	}

	if (!ret) {
		return 0;
	}

	return -EINVAL;
}

