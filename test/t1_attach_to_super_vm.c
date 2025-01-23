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

int main(int argc, char **argv) {
	int fd, ret;
	int a;
	char *addr;
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
	donor.addr = 0;
	ret = ioctl(fd, IOCTL_CONNECT_EM, &donor);
	if (ret) {
		fprintf(stderr, "failed to connect elastic memory\n");
		exit(-1);
	}
	printf("connect\n");

	addr = mmap(0, donor.size << 20, PROT_READ|PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		/*close(fd);*/
		fprintf(stderr, "%s\n", strerror(errno));
		/*exit(1);*/
	} else {
		fprintf(stderr, "mmap size: %ld MiB\n", donor.size);
	}

	a = *(int *)(addr + (4096*20000));
	/*sleep(10);*/
	printf("memory content: offset:4096*20000 value: %d\n", a);

	close(fd);

	return 0;
}
	
