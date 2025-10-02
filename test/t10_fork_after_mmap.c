//#define _WINDOWS
//#define _LINUX

#include <stdio.h>
#include <stdlib.h>
#include <wait.h>

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

#define TEST_MAP_SHARED

#define MEMSIZE (8ULL << 30)

#define ITER_UNIT (4*1024/sizeof(unsigned long long))
#define REPEAT 10
#define CHECK_REPEAT 3
#define SHOW_UNIT 100000

unsigned long memsize = MEMSIZE;
enum {
	MEM_ALLOC_MMAP,
	MEM_ALLOC_MALLOC,
} mem_alloc_type = MEM_ALLOC_MMAP;
int write_over_read = 0;

#define IDX_TO_MB(i) (((i) * ITER_UNIT * sizeof(unsigned long long)) >> 20)
#define IDX_TO_GB(i) (((i) * ITER_UNIT * sizeof(unsigned long long)) >> 30)

#ifdef _WINDOWS
#define GET_TIME(t) _ftime(t);
typedef struct _timeb TIME_T;

unsigned long get_elaped_time_usec(TIME_T *now, TIME_T *prev)
{
	unsigned long t;
	t = (now->millitm - prev->millitm + 
			(now->millitm < prev->millitm ? 1000 : 0));
	return t;
}

unsigned long get_elaped_time_sec(TIME_T *now, TIME_T *prev)
{
	unsigned long t;
	t = (now->time - prev->time - (now->millitm < prev->millitm ? 1 : 0));
	return t;
}
#else
#define GET_TIME(t) gettimeofday(t, NULL);
typedef struct timeval TIME_T;

unsigned long get_elaped_time_usec(TIME_T *now, TIME_T *prev)
{
	unsigned long t;
	t = ((now->tv_usec - prev->tv_usec + 
				(now->tv_usec < prev->tv_usec ? 
				 1000000 : 0)) + 500) / 1000;
	return t;
}

unsigned long get_elaped_time_sec(TIME_T *now, TIME_T *prev)
{
	unsigned long t;
	t = (now->tv_sec - prev->tv_sec - (now->tv_usec < prev->tv_usec ?1: 0));
	return t;
}
#endif

void 
fill_data(unsigned long long *mem, unsigned long end_idx, char *label, int re,
		unsigned long base)
{
	unsigned long long curr = 0;
	TIME_T start, prev, now;

	GET_TIME(&start);
	prev = start;
	while (curr < end_idx) {
		mem[curr] = curr + base;
		curr += ITER_UNIT;
		if (curr % (SHOW_UNIT*ITER_UNIT) == 0) {
			GET_TIME(&now);
			printf("[%s-%04d]curr: %10lldMB %5lldGB time: %ld.%03lds\n",
					label, re,
					(curr * sizeof(unsigned long long)) >> 20,
					(curr * sizeof(unsigned long long)) >> 30,
					get_elaped_time_sec(&now, &prev),
					get_elaped_time_usec(&now, &prev)
			      );
			fflush(stdout);
			prev = now;
		}
	}

	GET_TIME(&now);
	fprintf(stderr, "[%s-%04d]END: %10lldMB %5lldGB time: %ld.%03lds\n",
			label, re,
			(curr * sizeof(unsigned long long)) >> 20,
			(curr * sizeof(unsigned long long)) >> 30,
			get_elaped_time_sec(&now, &start),
			get_elaped_time_usec(&now, &start));
	fflush(stdout);
}

void check_data(unsigned long long *mem, char *label, int re, int subre,
		unsigned long long *checked, unsigned long long *total,
		unsigned long start_idx, unsigned long end_idx, int g,
		unsigned long base, int print_unit)
{
	unsigned long long curr = start_idx;
	unsigned long pred_value;
	TIME_T start, prev, now;
	int gen, next_gen;
	char dummy;

	GET_TIME(&start);
	prev = start;
	gen = write_over_read? g: 0;
	next_gen = write_over_read? g+1: 0;

	while (curr < end_idx) {
		(*total)++;
		if (write_over_read == 0 || ((curr % write_over_read) != 0))
			pred_value = base + curr;
		else
			pred_value = base + curr + gen;

		if (mem[curr] == pred_value) {
			(*checked)++;
		} else {
			fprintf(stderr, "[%s-%04d-%d]MEMORY_ERROR at %llx (@%lldMB) addr: 0x%p value: %lld expected: %ld base: %ld gen: %d\n",
					label, re, subre, curr,
					(curr * sizeof(unsigned long long)) >> 20,
					&mem[curr], mem[curr], pred_value, base, gen);
			fprintf(stderr, "continue");
			scanf("%c", &dummy);
		}

		if (write_over_read && ((curr % write_over_read) == 0))
			mem[curr] = (curr + next_gen);

		curr += ITER_UNIT;
		if (curr % (print_unit*ITER_UNIT) == 0) {
			GET_TIME(&now);
			printf("[%s-%04d-%d]checked: %10lldMB/%10lldMB "
					"(%5lldGB/%5lldGB) time: %ld.%03lds\n",
					label, re, subre,
					IDX_TO_MB(*checked), IDX_TO_MB(*total),
					IDX_TO_GB(*checked), IDX_TO_GB(*total),
					get_elaped_time_sec(&now, &prev),
					get_elaped_time_usec(&now, &prev));
			fflush(stdout);
			prev = now;
		}
	}

	GET_TIME(&now);
	fprintf(stderr, "[%s-%04d-%d]END: checked: %10lldMB/%10lldMB "
			"(%5lldGB/%5lldGB) time: %ld.%03lds\n",
			label, re, subre,
			IDX_TO_MB(*checked), IDX_TO_MB(*total),
			IDX_TO_GB(*checked), IDX_TO_GB(*total),
			get_elaped_time_sec(&now, &start),
			get_elaped_time_usec(&now, &start));
	fflush(stdout);
}

unsigned long long *mem_alloc(char *label, unsigned long long size)
{
	unsigned long long *mem;

	if (mem_alloc_type == MEM_ALLOC_MALLOC)
#ifdef TEST_MAP_SHARED
	{
		fprintf(stderr, "ERROR: malloc() cannot test MAP_SHARED");
		return 0;
	}
#else
		return malloc(size);
#endif

	mem = (unsigned long long *)mmap(NULL, size,
			PROT_READ|PROT_WRITE,
#ifdef TEST_MAP_SHARED
			MAP_ANONYMOUS|MAP_SHARED,
#else
			MAP_ANONYMOUS|MAP_PRIVATE,
#endif
			-1, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "exit: %s\n", strerror(errno));
		exit(1);
	} else {
		fprintf(stderr, "[%s] mmap addr: %llx size: %lld MiB\n", label, 
				(unsigned long long) mem, size >> 20);
	}
	return mem;
}

void mem_free(unsigned long long *mem, unsigned long size)
{
	if (mem_alloc_type == MEM_ALLOC_MALLOC)
		return free(mem);

	munmap(mem, size);
}


void usage(char *prog)
{
	fprintf(stderr, "usage: %s [OPTIONS]\n", prog);
	fprintf(stderr, "[OPTIONS]\n");
	fprintf(stderr, "-m <total memory size>\n");
	fprintf(stderr, "-a <memory allocator type: 0:mmap, 1:malloc>\n");
	exit(-1);
}

void parse_args(int argc, char **argv)
{
	int c;
	opterr = 0;
	while ((c = getopt(argc, argv, "m:a:")) != -1) {
		switch (c) {
		case 'm':
			memsize = atol(optarg);
			break;
		case 'a':
			mem_alloc_type = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			break;
		}
	}
}

int main(int argc, char **argv) 
{
	unsigned long long *mem;
	unsigned long long checked, total, end_idx;

	pid_t pid;
	int status;
	char label[] = "parent";

	parse_args(argc, argv);

	end_idx = memsize / sizeof(unsigned long long);

	mem = mem_alloc(label, memsize);
	if (mem == NULL) {
		perror("not enough memory: ");
		exit(-1);
	}

	fill_data(mem, end_idx, label, 0, 0ul);

	total = 0;
	checked = 0;
	check_data(mem, label, 0, 0, &checked, &total, 0, end_idx, 0, 0ul, SHOW_UNIT);

	/*fprintf(stdout, "press a key: ");*/
	/*getchar();*/
	//exit(0);

	/*total = 0;*/
	/*checked = 0;*/
	/*check_data(mem, label, 0, 1, &checked, &total, 0, end_idx, 0, SHOW_UNIT);*/

	pid = fork();
	if (pid == 0) {
		char label[] = "child";

		/*fprintf(stdout, "press a key: ");*/
		/*getchar();*/

		/*fill_data(mem, end_idx, label, 0, 100ul);*/
		total = 0;
		checked = 0;
		check_data(mem, label, 0, 0, &checked, &total, 0, end_idx, 0, 0ul, SHOW_UNIT);

		fill_data(mem, end_idx, label, 0, 100ul);

		fprintf(stderr, "this is a child process pid:%d\n", getpid());
		/*fprintf(stdout, "press a key: ");*/
		/*getchar();*/

		/*total = 0;*/
		/*checked = 0;*/
		/*check_data(mem, label, 0, 1, &checked, &total, 0, end_idx, 0, 100ul, SHOW_UNIT);*/

		/*total = 0;*/
		/*checked = 0;*/
		/*check_data(mem, label, 0, 2, &checked, &total, 0, end_idx, 0, SHOW_UNIT);*/

		return 0;
	}

	if (pid < 0)
		fprintf(stderr, "failed to fork\n");
	else if (waitpid(pid, &status, 0) < 0)
		fprintf(stderr, "waitpid for %d error\n", pid);

	/*fprintf(stdout, "press a key: ");*/
	/*getchar();*/

	/*fill_data(mem, end_idx, label, 0, 100ul);*/
	/*fill_data(mem, end_idx, label, 0, 0ul);*/
	total = 0;
	checked = 0;
#ifdef TEST_MAP_SHARED
	check_data(mem, label, 0, 0, &checked, &total, 0, end_idx, 0, 100ul, SHOW_UNIT);
#else
	check_data(mem, label, 0, 0, &checked, &total, 0, end_idx, 0, 0ul, SHOW_UNIT);
#endif

	fprintf(stderr, "this is a parent process pid:%d\n", getpid());

	mem_free(mem, memsize);

	return 0;
}
