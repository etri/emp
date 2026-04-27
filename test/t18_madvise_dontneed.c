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
#include "../include/emp_ioctl.h"


#define MEMSIZE (24ULL << 30)
#define CACHESIZE (8ULL << 30)

#define ITER_UNIT (4*1024/sizeof(unsigned long long))

#define SIZE_TO_GB(size) ((size) >> 30)
#define MEMSIZE_GB SIZE_TO_GB(MEMSIZE)

#define time_diff_sec(start, now) ((now).tv_sec - (start).tv_sec - ((now).tv_usec < (start).tv_usec ? 1 : 0))
#define time_diff_ms(start, now) (((now.tv_usec - start.tv_usec + (now.tv_usec < start.tv_usec ? 1000000 : 0)) + 500) / 1000)

int opt_dontwait = 0;
static inline void wait_any_key(void) {
	if (opt_dontwait) {
		printf("-----------------------------------------------------\n");
		return;
	}
	printf("Enter Any Key...\n");
	getchar();
}

#if 0
unsigned long delay(void) {
	unsigned long i, ret = 0;
	for (i = 0; i < 100; i++)
		ret += i;
	return ret;
}
#else
#define delay() do {} while (0)
#endif

/*
 * read_and_check_range: same as read_and_check but limited to [start_off, end_off).
 * Useful to measure access latency right after MADV_DONTNEED on a specific range.
 */
void read_and_check_range(register unsigned long long *mem,
					const unsigned long prev_factor,
					const unsigned long new_factor,
					unsigned long long start_off,
					unsigned long long end_off)
{
	register unsigned long long curr, checked = 0, failed = 0;
	register unsigned long long start_idx, end_idx;
	struct timeval start, now;

	start_idx = start_off / sizeof(unsigned long long);
	end_idx = end_off / sizeof(unsigned long long);

	gettimeofday(&start, NULL);
	for (curr = start_idx; curr < end_idx; curr+=ITER_UNIT) {
		if (mem[curr] == prev_factor * curr)
			checked++;
		else
			failed++;
		mem[curr] = new_factor * curr;
		delay();
	}
	gettimeofday(&now, NULL);
	printf("[READ&CHECK] off: %lldGB-%lldGB time: %ld.%03lds num_failed: %lld/%lld\n",
				SIZE_TO_GB(start_off), SIZE_TO_GB(end_off),
				time_diff_sec(start, now), time_diff_ms(start, now),
				failed, checked + failed);
}

void read_and_check(register unsigned long long *mem, const unsigned long prev_factor, const unsigned long new_factor)
{
	read_and_check_range(mem, prev_factor, new_factor, 0, MEMSIZE);
}

void fill_mem(register unsigned long long *mem, const unsigned long factor)
{
	register unsigned long long curr, end_idx;
	end_idx = MEMSIZE / sizeof(unsigned long long);
	for (curr = 0; curr < end_idx; curr+=ITER_UNIT)
		mem[curr] = factor * curr;
}


int main(int argc, char **argv)
{
	register unsigned long long curr, end_idx;
	register unsigned long long *mem;
	struct timeval start, now;
	register unsigned long factor = 2;

	if (argc > 1 && strcmp(argv[1], "--dontwait") == 0)
		opt_dontwait = 1;

	end_idx = MEMSIZE / sizeof(unsigned long long);

	mem = (unsigned long long *)mmap(NULL, MEMSIZE,
			PROT_READ|PROT_WRITE,
			MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "exit: %s\n", strerror(errno));
		exit(1);
	} else {
		fprintf(stderr, "mmap size: %lld MiB\n", MEMSIZE >> 20);
	}
	gettimeofday(&start, NULL);
	fill_mem(mem, factor);
	gettimeofday(&now, NULL);
	printf("[WRITE] %lldGB time: %ld.%03lds\n", MEMSIZE_GB, time_diff_sec(start, now), time_diff_ms(start, now));
	fflush(stdout);
	wait_any_key();

	/* CHECK: warm baseline before DONTNEED */
	gettimeofday(&start, NULL);
	read_and_check(mem, factor, factor);
	gettimeofday(&now, NULL);
	printf("[BASELINE] %lldGB time: %ld.%03lds\n",
				SIZE_TO_GB(MEMSIZE), time_diff_sec(start, now), time_diff_ms(start, now));
	fflush(stdout);
	wait_any_key();

	/* CHECK_RANGE: ensure CACHESIZE on cache */
	gettimeofday(&start, NULL);
	read_and_check_range(mem, factor, factor, 0, CACHESIZE);
	gettimeofday(&now, NULL);
	printf("[WARM] %lldGB time: %ld.%03lds\n",
				SIZE_TO_GB(CACHESIZE), time_diff_sec(start, now), time_diff_ms(start, now));
	fflush(stdout);
	wait_any_key();

	/************* DONTNEED CACHESIZE/2 ***************
	 * Positive size: blocks are moved to inactive list without force.
	 * Pages may or may not be reclaimed immediately depending on
	 * block state (e.g., GPA_PREFETCHED blocks are skipped). */
	gettimeofday(&start, NULL);
	madvise(mem, CACHESIZE/2, MADV_EMP_DONTNEED);
	gettimeofday(&now, NULL);
	printf("[DONTNEED] %lldGB time: %ld.%03lds\n",
				SIZE_TO_GB(CACHESIZE), time_diff_sec(start, now), time_diff_ms(start, now));
	fflush(stdout);
	wait_any_key();
	
	/* CHECK_RANGE: fetch another part of data on cache */
	gettimeofday(&start, NULL);
	read_and_check_range(mem, factor, factor, CACHESIZE, CACHESIZE + CACHESIZE/2);
	gettimeofday(&now, NULL);
	printf("[LOAD_OTHER] %lldGB time: %ld.%03lds\n",
				SIZE_TO_GB(CACHESIZE/2), time_diff_sec(start, now), time_diff_ms(start, now));
	fflush(stdout);
	wait_any_key();

	/* CHECK on out of DONTNEED range: should be fast */
	gettimeofday(&start, NULL);
	read_and_check_range(mem, factor, factor, CACHESIZE/2, CACHESIZE);
	gettimeofday(&now, NULL);
	printf("[OUT_OF_DONTNEED] %lldGB time: %ld.%03lds\n",
				SIZE_TO_GB(CACHESIZE/2), time_diff_sec(start, now), time_diff_ms(start, now));
	fflush(stdout);
	wait_any_key();
	
	/* CHECK on DONTNEED range: should be slow */
	gettimeofday(&start, NULL);
	read_and_check_range(mem, factor, factor, 0, CACHESIZE/2);
	gettimeofday(&now, NULL);
	printf("[DONTNEED_RANGE] %lldGB time: %ld.%03lds\n",
				SIZE_TO_GB(CACHESIZE/2), time_diff_sec(start, now), time_diff_ms(start, now));
	fflush(stdout);
	wait_any_key();

	munmap(mem, MEMSIZE);

	return 0;
}
