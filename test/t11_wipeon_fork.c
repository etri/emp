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

#define MEMSIZE (1ULL << 30)

#define ITER_UNIT (4*1024/sizeof(unsigned long long))
#define REPEAT 10
#define CHECK_REPEAT 3
#define SHOW_UNIT 100000

unsigned long memsize = MEMSIZE;
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
		//		printf("FILL_DATA: mem[%d]=%d\n", curr, curr+base);
		curr += ITER_UNIT;
		if (curr % (SHOW_UNIT*ITER_UNIT) == 0) {
			GET_TIME(&now);
			printf("[%s-%04d]fill %10lldMB %5lldGB time: %ld.%03lds\n",
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
	fprintf(stderr, "[%s-%04d]END: fill: %10lldMB %5lldGB time: %ld.%03lds\n",
			label, re,
			(curr * sizeof(unsigned long long)) >> 20,
			(curr * sizeof(unsigned long long)) >> 30,
			get_elaped_time_sec(&now, &start),
			get_elaped_time_usec(&now, &start));
	fflush(stdout);
}

void check_data_iszero(unsigned long long *mem, char *label, int re, int subre,
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
		pred_value = 0;
		if (mem[curr] == pred_value) {
			(*checked)++;
		} else {
			fprintf(stderr, "[%s-%04d-%d]MEMORY_ERROR at %llx (@%lldMB) (0x%p %llx)\n",
					label, re, subre, curr,
					(curr * sizeof(unsigned long long)) >> 20,
					&mem[curr], mem[curr]);
			fprintf(stderr, "continue");
			//			scanf("%c", &dummy);
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

void check_data_different(unsigned long long *mem, char *label, int re, int subre,
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

		if (mem[curr] != pred_value) {
			(*checked)++;
		} else {
			fprintf(stderr, "[%s-%04d-%d]MEMORY_ERROR at %llx (@%lldMB) (0x%p %llx)\n",
					label, re, subre, curr,
					(curr * sizeof(unsigned long long)) >> 20,
					&mem[curr], mem[curr]);
			fprintf(stderr, "continue");
			//			scanf("%c", &dummy);
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


void check_data_same(unsigned long long *mem, char *label, int re, int subre,
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
			fprintf(stderr, "[%s-%04d-%d]MEMORY_ERROR at %llx (@%lldMB) (0x%p %llx)\n",
					label, re, subre, curr,
					(curr * sizeof(unsigned long long)) >> 20,
					&mem[curr], mem[curr]);
			fprintf(stderr, "continue");
			//			scanf("%c", &dummy);
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
	int ret;

	mem = (unsigned long long *)mmap(NULL, size,
			PROT_READ|PROT_WRITE,
			MAP_ANONYMOUS|MAP_PRIVATE,
			//			MAP_ANONYMOUS|MAP_SHARED,
			-1, 0);

	if (mem == MAP_FAILED) {
		fprintf(stderr, "exit: %s\n", strerror(errno));
		exit(1);
	} else {
		fprintf(stderr, "[%s] mmap addr: %llx size: %lld MiB\n", label,
				(unsigned long long) mem, size >> 20);
	}

	/* (Note)
	   MADV_WIPEONFORK (since Linux 4.14)
	   Present the child process with zero-filled memory in this
	   range after a fork(2).  This is useful in forking servers
	   in order to ensure that sensitive per-process data (for
	   example, PRNG seeds, cryptographic secrets, and so on) is
	   not handed to child processes.

	   The MADV_WIPEONFORK operation can be applied only to
	   private anonymous pages (see mmap(2)).

	   Within the child created by fork(2), the MADV_WIPEONFORK
	   setting remains in place on the specified address range.
	   This setting is cleared during execve(2).
	*/
	ret = madvise(mem, size, MADV_WIPEONFORK);
	if (ret) {
		fprintf(stderr, "madvise(0x%llx, 0x%llx, MADV_WIPEONFORK) returns %d. errno: %d\n",
				(unsigned long long) mem, size, ret, errno);
		munmap(mem, size);
		mem = NULL;
	}

	return mem;
}

void mem_free(unsigned long long *mem, unsigned long size)
{
	munmap(mem, size);
}


void usage(char *prog)
{
	fprintf(stderr, "usage: %s [OPTIONS]\n", prog);
	fprintf(stderr, "[OPTIONS]\n");
	fprintf(stderr, "-m <total memory size>\n");
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
				printf("memsize = 0x%lx\n", memsize);
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


	total = 0;
	checked = 0;
	printf("\n1. PARENT WRITE the value, PARENT CHECK it again\n");
	fprintf(stderr, "this is a parent process pid:%d\n", getpid());
	fill_data(mem, end_idx, label, 1, 50ul);
	check_data_same(mem, label, 1, 0, &checked, &total, 0, end_idx, 0, 50ul, SHOW_UNIT);

	/*fprintf(stdout, "press a key: ");*/
	/*getchar();*/

	pid = fork();
	if (pid == 0) {
		char label[] = "child";

		/*fprintf(stdout, "press a key: ");*/
		/*getchar();*/

		total = 0;
		checked = 0;
		printf("\n2. value WRITTEN BY PARENT, READ AS ZERO to CHILD (seen as ZERO)\n");
		fprintf(stderr, "this is a child process pid:%d\n", getpid());
		check_data_iszero(mem, label, 2, 0, &checked, &total, 0, end_idx, 0, 50ul, SHOW_UNIT);

		printf("\n3. CHILD WRITE the value, CHILD CHECK it again\n");
		fprintf(stderr, "this is a child process pid:%d\n", getpid());
		fill_data(mem, end_idx, label, 3, 100ul);
		check_data_same(mem, label, 3, 0, &checked, &total, 0, end_idx, 0, 100ul, SHOW_UNIT);

		/*fprintf(stdout, "press a key: ");*/
		/*getchar();*/

		return 0;
	}

	if (pid < 0)
		fprintf(stderr, "failed to fork\n");
	else if (waitpid(pid, &status, 0) < 0)
		fprintf(stderr, "waitpid for %d error\n", pid);

	/*fprintf(stdout, "press a key: ");*/
	/*getchar();*/

	total = 0;
	checked = 0;

	printf("\n4. value WRITTEN BY CHILD, PARENT CANNOT READ\n");
	fprintf(stderr, "this is a parent process pid:%d\n", getpid());
	check_data_different(mem, label, 4, 0, &checked, &total, 0, end_idx, 0, 100ul, SHOW_UNIT);

	mem_free(mem, memsize);

	return 0;
}
