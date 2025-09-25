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
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

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
	int re = 0, i;
#ifdef _WINDOWS
	struct _timeb start, prev, now;
#else
	struct timeval start, prev, now;
#endif

	// mmap to attach a memory region
	for (re = 0; re < REPEAT; re++) {
		curr = 0;
		end_idx = MEMSIZE / sizeof(unsigned long long);

#define ALLOC_MMAP
#ifdef ALLOC_MMAP
		mem = (unsigned long long *)mmap(NULL, MEMSIZE,
				PROT_READ|PROT_WRITE,
				MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
		if (mem == MAP_FAILED) {
			fprintf(stderr, "exit: %s\n", strerror(errno));
			exit(1);
		} else {
			fprintf(stderr, "mmap size: %lld MiB\n", MEMSIZE >> 20);
		}
#else
		mem = malloc(MEMSIZE);
#endif
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

#ifdef ALLOC_MMAP
		munmap(mem, MEMSIZE >> 1);
		munmap(mem + (MEMSIZE >> 1), MEMSIZE >> 1);
#else
		free(mem);
#endif
	}

	return 0;
}
