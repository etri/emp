#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#define __USE_DONOR_DEV_TYPE_STR__
#include <emp/emp_ioctl.h>
#include <emp/libemp.h>
#include <numa.h>

#include <unistd.h>

#define	MINIMUM_LOCAL_CACHE_SIZE	(4 << 10)	// MiB


static enum donor_dev_type emp_get_donor_dev_type(char *path)
{
	if (strncmp(path, "dram", 4) == 0 ||
			strncmp(path, "DRAM", 4) == 0)
		return DONOR_DEV_DRAM;

	if (strncmp(path, "memdev", 6) == 0 ||
			strncmp(path, "MEMDEV", 6) == 0)
		return DONOR_DEV_MEMDEV;

	if (strstr(path, "/dev/nvme") == path)
		return DONOR_DEV_NVME;

	/* ramdisk support */
	if (strstr(path, "/dev/ram") == path)
		return DONOR_DEV_NVME;

	/* pmem is supported as a nvme device */
	if (strstr(path, "/dev/pmem") == path)
		return DONOR_DEV_NVME;

	return DONOR_DEV_RDMA;
}

static void populate_memory_region(void *__ptr, size_t size)
{
	long *ptr = (long *) __ptr;
	long *end = (long *) (((char *) __ptr) + size);
	for ( ; ptr < end; ptr += (4096/sizeof(long)))
		*ptr = 0L;
	printf("Memory region 0x%016lx ~ 0x%016lx is populated\n",
			(unsigned long) __ptr,
			(unsigned long) end);
}

size_t get_container_memory_limit(void) {
	FILE *fp = NULL;
	char buffer[256];
	size_t limit = -1;

	// cgroup v2: /sys/fs/cgroup/memory.max
	fp = fopen("/sys/fs/cgroup/memory.max", "r");
	if (fp == NULL) {
		// fallback to cgroup v1
		fp = fopen("/sys/fs/cgroup/memory/memory.limit_in_bytes", "r");
	}

	if (fp != NULL) {
		if (fgets(buffer, sizeof(buffer), fp) != NULL) {
			buffer[strcspn(buffer, "\n")] = 0;  // remove newline
			if (strcmp(buffer, "max") == 0) {
				limit = -1;  // unlimited
			} else {
				limit = (size_t) (strtoll(buffer, NULL, 10) >> 20); // MiB
			}
		}
		fclose(fp);
	} else {
		print_verbose("\n");
		fprintf(stderr, "libemp.so: [ERROR] Failed to open cgroup memory file. Note: \"dram:-1\" indicates that EMP will use the cgroup-assigned memory limit.\n");
	}

	return limit;
}

size_t get_host_total_memory(void) {
	long pages = sysconf(_SC_PHYS_PAGES);
	long page_size = sysconf(_SC_PAGE_SIZE);

	return pages * page_size;  // byte
}

int emp_setup_media(int fd, const char *__path)
{
	struct donor_info *donor;
	int ret;
	char *token, *subtoken, *str, *c, term;
	char *saveptr1, *saveptr2 = NULL;
	int path_len;
	enum donor_dev_type dev_type;
	char *path;
	size_t limit;

	if (__path == NULL)
		return -EINVAL;

	path = strdup(__path);
	if (path == NULL)
		return -errno;

	term = '\0';
	str = (char *)path;
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

	print_verbose("emp memory: %s\n", str);
	while ((token = strtok_r(str, "|", &saveptr1))) {
		subtoken = strtok_r(token, ":", &saveptr2);
		path_len = strlen(subtoken) + 1;

		donor = malloc(sizeof(struct donor_info) + path_len);
		if (donor == NULL) {
			ret = -ENOMEM;
			break;
		}
		donor->path_len = path_len;
		strncpy(donor->path, subtoken, path_len);

		dev_type = emp_get_donor_dev_type(donor->path);
		print_verbose("memory_node: type: %s path: %s", 
				donor_dev_type_str[dev_type],
				((dev_type != DONOR_DEV_DRAM)? 
				 donor->path : "DRAM"));
		donor->dev_type = dev_type;

		subtoken = strtok_r(NULL, ":", &saveptr2);
		print_verbose(", size: %s", subtoken);
		donor->size = atol(subtoken);

		subtoken = strtok_r(NULL, ":", &saveptr2);
		switch (dev_type) {
		case DONOR_DEV_RDMA:
		case DONOR_DEV_NVME:
		case DONOR_DEV_PMEM:
		case DONOR_DEV_MEMDEV:
			if (!subtoken) {
				ret = -EINVAL;
				break;
			}

			donor->loc = 0; // initialization
			if (dev_type == DONOR_DEV_RDMA) {
				donor->addr = inet_addr(donor->path);
				donor->port = atoi(subtoken);
				print_verbose(", port: %d", donor->port);
			} else if (dev_type == DONOR_DEV_NVME
					|| dev_type == DONOR_DEV_PMEM) {
				donor->base = atoi(subtoken);
				print_verbose(", base: 0x%x", donor->base);
			} else if (dev_type == DONOR_DEV_MEMDEV) {
				donor->node = atoi(subtoken);
				if (subtoken[0] == '-') {
					donor->node = -donor->node;
					donor->ptr = numa_alloc_onnode(donor->size << 20, donor->node);
					if (donor->ptr == NULL) {
						ret = -ENOMEM;
						break;
					}
					print_verbose(", node: %d ptr: %p", donor->node, donor->ptr);
					populate_memory_region(donor->ptr, donor->size << 20);
				} else {
					print_verbose(", node: %d ptr: %p", donor->node, NULL);
				}
			}
			print_verbose("\n");

			ret = ioctl(fd, IOCTL_CONN_DONOR, donor);
			break;
		case DONOR_DEV_DRAM:
			if (donor->size == (size_t)-1) {
				limit = get_container_memory_limit();
				if (limit != (size_t)-1) {
					donor->size = limit; // MiB
					print_verbose(" \u2192 use container memory limit: %ld", donor->size);
				} else {
					return -ENOMEM;
				}
			}
			print_verbose("\n");

			if (donor->size < (size_t) MINIMUM_LOCAL_CACHE_SIZE) {
				fprintf(stderr, "libemp.so: [ERROR] Minimum local cache size should be larger than %d GiB\n", MINIMUM_LOCAL_CACHE_SIZE >> 10);
				return -ENOMEM;
			}

			ret = ioctl(fd, IOCTL_SET_DRAM, donor);
			break;
		}
		free(donor);

		if (ret)
			break;
		str = NULL;
	}

	free(path);

	if (!ret)
		ret = ioctl(fd, IOCTL_FINI_CONN, 0);


	if (!ret)
		return 0;

	return -EINVAL;
}

