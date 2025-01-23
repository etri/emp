//#define _WINDOWS
//#define _LINUX

#include <stdio.h>
#include <stdlib.h>

#ifdef _WINDOWS
#include <sys/timeb.h>
#include <time.h>
#else // _LINUX
#include <sys/time.h>
#endif
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "../include/emp_ioctl.h"

#define MEMSIZE (80ULL << 30)

#define ITER_UNIT (4*1024/sizeof(unsigned long long))
#define REPEAT 1
#define CHECK_REPEAT 1
#define SHOW_UNIT 100000

int main(int argc, char **argv) 
{
	unsigned long long curr, end_idx;
	unsigned long long *mem;
	unsigned long long checked, total;
	char nvme[] = "/dev/nvme4n1";
	int re = 0, i, ret, empfd;
	struct donor_info donor;
	char *addr;
#ifdef _WINDOWS
	struct _timeb start, prev, now;
#else
	struct timeval start, prev, now;
#endif

	empfd = open("/dev/emp", O_RDWR);
	if (empfd < 0) {
		fprintf(stderr, "%s\n", strerror(errno));
		exit(1);
	}

	// set dram capacity
	memset(&donor, 0, sizeof(donor));
	donor.size = ((size_t)24 << 10);
	ret = ioctl(empfd, IOCTL_SET_DRAM, &donor);
	if (ret) {
		fprintf(stderr, "failed to set dram - %ld MiB\n", donor.size);
		exit(-1);
	}

	// set 2nd tier memory
	memset(&donor, 0, sizeof(donor));
	strncpy(donor.path, nvme, sizeof(nvme));
	donor.path_len = sizeof(nvme);
	donor.dev_type = DONOR_DEV_NVME;
	donor.base = 2;
	donor.size = (MEMSIZE >> 20);
	ret = ioctl(empfd, IOCTL_CONN_DONOR, &donor);
	if (ret) {
		fprintf(stderr, "failed to connect 2nd tier memory\n");
		close(empfd);
		exit(-1);
	}

	// mmap to attach a memory region
	addr = mmap(0, MEMSIZE, PROT_READ|PROT_WRITE, MAP_SHARED, empfd, 0);
	if (addr == MAP_FAILED) {
		fprintf(stderr, "%s - exit\n", strerror(errno));

		close(empfd);
		exit(1);
	} else {
		fprintf(stderr, "mmap size: %ld MiB\n", donor.size);
	}

	for (re = 0; re < REPEAT; re++) {
		curr = 0;
		end_idx = MEMSIZE / sizeof(unsigned long long);
		/*mem = (unsigned long long *)malloc(MEMSIZE);*/
		mem = (unsigned long long *)addr;

#ifdef _WINDOWS
		_ftime(&start);
#else
		gettimeofday(&start, NULL);
#endif
		prev = start;
		while (curr < end_idx) {
			mem[curr] = curr;
			curr+=ITER_UNIT;
			if (curr % (SHOW_UNIT*ITER_UNIT) == 0) {
#ifdef _WINDOWS
				_ftime(&now);
#else
				gettimeofday(&now, NULL);
#endif
				printf("[%04d]curr: %10lldMB %5lldGB time: %ld.%03lds\n", re,
							(curr * sizeof(unsigned long long)) >> 20, (curr * sizeof(unsigned long long)) >> 30,
#ifdef _WINDOWS
							now.time - prev.time - (now.millitm < prev.millitm ? 1 : 0),
							now.millitm - prev.millitm + (now.millitm < prev.millitm ? 1000 : 0)
#else
							now.tv_sec - prev.tv_sec - (now.tv_usec < prev.tv_usec ? 1 : 0),
							((now.tv_usec - prev.tv_usec + (now.tv_usec < prev.tv_usec ? 1000000 : 0)) + 500) / 1000
#endif
				);
				fflush(stdout);
				prev = now;
			}
		}

#ifdef _WINDOWS
				_ftime(&now);
#else
				gettimeofday(&now, NULL);
#endif
		printf("[%04d]END: %10lldMB %5lldGB time: %ld.%03lds\n", re,
					(curr * sizeof(unsigned long long)) >> 20, (curr * sizeof(unsigned long long)) >> 30,
#ifdef _WINDOWS
					now.time - start.time - (now.millitm < start.millitm ? 1 : 0),
					now.millitm - start.millitm + (now.millitm < start.millitm ? 1000 : 0)
#else
					now.tv_sec - start.tv_sec - (now.tv_usec < start.tv_usec ? 1 : 0),
					((now.tv_usec - start.tv_usec + (now.tv_usec < start.tv_usec ? 1000000 : 0)) + 500) / 1000
#endif
				);
		fflush(stdout);

		for (i = 0; i < CHECK_REPEAT; i++) {
			curr = 0;
			checked = 0;
			total = 0;
#ifdef _WINDOWS
		_ftime(&start);
#else
		gettimeofday(&start, NULL);
#endif
			prev = start;
			while (curr < end_idx) {
				total++;
				if (mem[curr] == curr)
					checked++;
				else {
					char dummy;
					printf("[%04d-%d]MEMORY_ERROR at %llx (@%lldMB) (0x%p %llx)\n", re, i, curr, (curr * sizeof(unsigned long long)) >> 20, &mem[curr], mem[curr]);
					printf("continue");
					scanf("%c", &dummy);					
				}

				curr+=ITER_UNIT;
				if (curr % (SHOW_UNIT*ITER_UNIT) == 0) {
#ifdef _WINDOWS
					_ftime(&now);
#else
					gettimeofday(&now, NULL);
#endif
					printf("[%04d-%d]checked: %10lldMB/%10lldMB (%5lldGB/%5lldGB)  time: %ld.%03lds\n", re, i,
								(checked * ITER_UNIT * sizeof(unsigned long long)) >> 20,
								(total * ITER_UNIT * sizeof(unsigned long long)) >> 20,
								(checked * ITER_UNIT * sizeof(unsigned long long)) >> 30,
								(total * ITER_UNIT * sizeof(unsigned long long)) >> 30,
#ifdef _WINDOWS
								now.time - prev.time - (now.millitm < prev.millitm ? 1 : 0),
								now.millitm - prev.millitm + (now.millitm < prev.millitm ? 1000 : 0)
#else
								now.tv_sec - prev.tv_sec - (now.tv_usec < prev.tv_usec ? 1 : 0),
								((now.tv_usec - prev.tv_usec + (now.tv_usec < prev.tv_usec ? 1000000 : 0)) + 500) / 1000
#endif
				);
					fflush(stdout);
					prev = now;
				}
			}

#ifdef _WINDOWS
			_ftime(&now);
#else
			gettimeofday(&now, NULL);
#endif
			printf("[%04d-%d]END: checked: %10lldMB/%10lldMB (%5lldGB/%5lldGB) time: %ld.%03lds\n", re, i,
								(checked * ITER_UNIT * sizeof(unsigned long long)) >> 20,
								(total * ITER_UNIT * sizeof(unsigned long long)) >> 20,
								(checked * ITER_UNIT * sizeof(unsigned long long)) >> 30,
								(total * ITER_UNIT * sizeof(unsigned long long)) >> 30,
#ifdef _WINDOWS
						now.time - start.time - (now.millitm < start.millitm ? 1 : 0),
						now.millitm - start.millitm + (now.millitm < start.millitm ? 1000 : 0)
#else
						now.tv_sec - start.tv_sec - (now.tv_usec < start.tv_usec ? 1 : 0),
						((now.tv_usec - start.tv_usec + (now.tv_usec < start.tv_usec ? 1000000 : 0)) + 500) / 1000
#endif
				);
			fflush(stdout);
		}

		/*free(mem);*/
	}

	close(empfd);

	return 0;
}
