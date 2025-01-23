#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <sys/ioctl.h>
#include "../include/emp_ioctl.h"

int main(int argc, char **argv)
{
	int fd, ret;
	int a;
	char *addr;
	char nvme[] = "/dev/nvme4n1";
	struct donor_info donor;

	fd = open("/dev/emp", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s\n", strerror(errno));
		exit(1);
	}

	memset(&donor, 0, sizeof(donor));
	donor.size = (24 << 10);
	ret = ioctl(fd, IOCTL_SET_DRAM, &donor);
	if (ret) {
		fprintf(stderr, "failed to set dram - 24GiB\n");
		exit(-1);
	}

	memset(&donor, 0, sizeof(donor));
	strncpy(donor.path, nvme, sizeof(nvme));
	donor.path_len = sizeof(nvme);
	donor.dev_type = DONOR_DEV_NVME;
	donor.base = 2;
	donor.size = 24 << 10;
	ret = ioctl(fd, IOCTL_CONN_DONOR, &donor);
	if (ret) {
		fprintf(stderr, "failed to connect 2nd tier memory\n");
		close(fd);
		exit(-1);
	}
	printf("connect\n");

	donor.size = (size_t)24 << 30;
	addr = mmap(0, donor.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		fprintf(stderr, "%s - exit\n", strerror(errno));
		close(fd);
		exit(1);
	} else {
		fprintf(stderr, "mmap size: %ld MiB\n", donor.size);
	}

	a = *(int *)(addr + (4096*20000));
	printf("memory content: offset:4096*20000 value: %d\n", a);

	close(fd);

	return 0;
}
	
