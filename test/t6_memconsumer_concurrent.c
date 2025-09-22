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
#include <pthread.h>

#define NUM_THREADS 1
#define MEMSIZE (64ULL << 30)

#define ITER_UNIT (4*1024/sizeof(unsigned long long))
#define REPEAT 10
#define CHECK_REPEAT 3
#define SHOW_UNIT 100000

int pause_before_mem_free = 0;
int repeat = REPEAT;
int check_repeat = CHECK_REPEAT;
int n_threads = NUM_THREADS;
unsigned long memsize = MEMSIZE;

void fill_data(unsigned long long *mem, unsigned long end_idx, char *label, int re)
{
	unsigned long long curr = 0;
#ifdef _WINDOWS
	struct _timeb start, prev, now;
#else
	struct timeval start, prev, now;
#endif

#ifdef _WINDOWS
	_ftime(&start);
#else
	gettimeofday(&start, NULL);
#endif
	prev = start;
	while (curr < end_idx) {
		mem[curr] = curr;
		curr += ITER_UNIT;
		if (curr % (SHOW_UNIT*ITER_UNIT) == 0) {
#ifdef _WINDOWS
			_ftime(&now);
#else
			gettimeofday(&now, NULL);
#endif
			printf("[%s-%04d]curr: %10lldMB %5lldGB time: %ld.%03lds\n",
					label, re,
					(curr * sizeof(unsigned long long)) >> 20,
					(curr * sizeof(unsigned long long)) >> 30,
#ifdef _WINDOWS
					(now.time - prev.time -
					 (now.millitm < prev.millitm ? 1 : 0)),
					(now.millitm - prev.millitm +
					 (now.millitm < prev.millitm ? 1000 : 0))
#else
					(now.tv_sec - prev.tv_sec -
					 (now.tv_usec < prev.tv_usec ? 1 : 0)),
					((now.tv_usec - prev.tv_usec + 
					  (now.tv_usec < prev.tv_usec ? 
					   1000000 : 0)) + 500) / 1000
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
	printf("[%s-%04d]END: %10lldMB %5lldGB time: %ld.%03lds\n", label, re,
			(curr * sizeof(unsigned long long)) >> 20,
			(curr * sizeof(unsigned long long)) >> 30,
#ifdef _WINDOWS
			(now.time - start.time - 
			 (now.millitm < start.millitm ? 1 : 0)),
			(now.millitm - start.millitm + 
			 (now.millitm < start.millitm ? 1000 : 0))
#else
			(now.tv_sec - start.tv_sec - 
			 (now.tv_usec < start.tv_usec ? 1 : 0)),
			(((now.tv_usec - start.tv_usec + 
			   (now.tv_usec < start.tv_usec ? 
			    1000000 : 0)) + 500) / 1000)
#endif
	      );
	fflush(stdout);
}

void check_data(unsigned long long *mem, unsigned long end_idx, char *label, int re, int subre)
{
	unsigned long long curr = 0;
	unsigned long long checked = 0;
	unsigned long long total = 0;
#ifdef _WINDOWS
	struct _timeb start, prev, now;
#else
	struct timeval start, prev, now;
#endif

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
			printf("[%s-%04d-%d]MEMORY_ERROR at %llx (@%lldMB) (0x%p %llx)\n",
					label, re, subre, curr,
					(curr * sizeof(unsigned long long)) >> 20,
					&mem[curr], mem[curr]);
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
			printf("[%s-%04d-%d]checked: %10lldMB/%10lldMB (%5lldGB/%5lldGB)"
					"time: %ld.%03lds\n", label, re, subre,
					(checked * ITER_UNIT * 
					 sizeof(unsigned long long)) >> 20,
					(total * ITER_UNIT * 
					 sizeof(unsigned long long)) >> 20,
					(checked * ITER_UNIT * 
					 sizeof(unsigned long long)) >> 30,
					(total * ITER_UNIT * 
					 sizeof(unsigned long long)) >> 30,
#ifdef _WINDOWS
					now.time - prev.time - 
					(now.millitm < prev.millitm ? 1 : 0),
					now.millitm - prev.millitm + 
					(now.millitm < prev.millitm ? 1000 : 0)
#else
					now.tv_sec - prev.tv_sec - 
					(now.tv_usec < prev.tv_usec ? 1 : 0),
					((now.tv_usec - prev.tv_usec + 
					  (now.tv_usec < prev.tv_usec ? 1000000 : 0)) + 500) / 1000
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
	printf("[%s-%04d-%d]END: checked: %10lldMB/%10lldMB (%5lldGB/%5lldGB) time: %ld.%03lds\n",
			label, re, subre,
			(checked * ITER_UNIT * sizeof(unsigned long long)) >> 20,
			(total * ITER_UNIT * sizeof(unsigned long long)) >> 20,
			(checked * ITER_UNIT * sizeof(unsigned long long)) >> 30,
			(total * ITER_UNIT * sizeof(unsigned long long)) >> 30,
#ifdef _WINDOWS
			now.time - start.time - 
			(now.millitm < start.millitm ? 1 : 0),
			now.millitm - start.millitm + 
			(now.millitm < start.millitm ? 1000 : 0)
#else
			now.tv_sec - start.tv_sec - 
			(now.tv_usec < start.tv_usec ? 1 : 0),
			((now.tv_usec - start.tv_usec + 
			  (now.tv_usec < start.tv_usec ? 1000000 : 0)) + 500) / 1000
#endif
	      );
	fflush(stdout);
}

#define ALLOC_MMAP
unsigned long long *mem_alloc(char *label, unsigned long long size)
{
	unsigned long long *mem;

#ifdef ALLOC_MMAP
	mem = (unsigned long long *)mmap(NULL, size,
			PROT_READ|PROT_WRITE,
			MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "exit: %s\n", strerror(errno));
		exit(1);
	} else {
		fprintf(stderr, "[%s] mmap size: %lld MiB\n", label, size >> 20);
	}
#else
	mem = malloc(size);
#endif
	return mem;
}

void mem_free(unsigned long long *mem, unsigned long size)
{
#ifdef ALLOC_MMAP
	munmap(mem, size);
#else
	free(mem);
#endif
}

struct thread_data {
	unsigned long thread_data_len;
	char label[4];
};

void *thread_func(void *data)
{
	unsigned long long curr, end_idx;
	unsigned long long *mem;
	unsigned long long checked, total;
	struct thread_data *tdata = (struct thread_data *)data;
	int re, i;
#ifdef _WINDOWS
	struct _timeb start, prev, now;
#else
	struct timeval start, prev, now;
#endif

	pid_t pid = getpid();
	pthread_t tid = pthread_self();;

	end_idx = tdata->thread_data_len / sizeof(unsigned long long);
	for (re = 0; re < repeat; re++) {

		mem = mem_alloc(tdata->label, tdata->thread_data_len);
		fill_data(mem, end_idx, tdata->label, re);

		for (i = 0; i < check_repeat; i++)
			check_data(mem, end_idx, tdata->label, re, i);

		if (pause_before_mem_free && (n_threads == 1)) {
			printf("press a key to continue: ");
			getchar();
		}
		mem_free(mem, tdata->thread_data_len);
	}
}

void usage(char *prog)
{
	fprintf(stderr, "usage: %s [OPTIONS]\n", prog);
	fprintf(stderr, "[OPTIONS]\n");
	fprintf(stderr, "-n <the number of threads>\n");
	fprintf(stderr, "-m <total memory size>\n");
	fprintf(stderr, "-r <the number of repeats>\n");
	fprintf(stderr, "-c <the number of check repeats>\n");
	fprintf(stderr, "-p pause before mem_free. only works with n==1\n");
	exit(-1);
}

int main(int argc, char **argv) 
{
	unsigned long memsize_per_thread;
	int i, thread_id, status;
	struct thread_data *tdata = NULL;
	pthread_t *p_thread = NULL;

	int c;
	opterr = 0;
	while ((c = getopt(argc, argv, "n:m:c:r:p")) != -1) {
		switch (c) {
		case 'n':
			n_threads = atoi(optarg);
			if (n_threads <= 0)
				usage(argv[0]);
			break;
		case 'm':
			memsize = atol(optarg);
			break;
		case 'c':
			check_repeat = atoi(optarg);
			break;
		case 'r':
			repeat = atoi(optarg);
			break;
		case 'p':
			pause_before_mem_free = 1;
			break;
		default:
			usage(argv[0]);
			break;
		}
	}
	memsize_per_thread = memsize / n_threads;

	p_thread = malloc(sizeof(pthread_t) * n_threads);
	if (p_thread == NULL)
		goto err;

	tdata = malloc(sizeof(struct thread_data) * n_threads);
	if (tdata == NULL)
		goto err;

	for (i = 0 ; i < n_threads; i++) {
		tdata[i].thread_data_len = memsize_per_thread;
		snprintf(tdata[i].label, 4, "t%d", i);
	}

	for (i = 0; i < n_threads; i++) {
		thread_id = pthread_create(&p_thread[i], NULL, thread_func,
					   &tdata[i]);
		if (thread_id < 0) {
			perror("thread create error : ");
			exit(0);
		}
	}

	for (i = 0; i < n_threads; i++)
		pthread_join(p_thread[i], (void **)&status);

	return 0;
err:
	if (p_thread)
		free(p_thread);
	if (tdata)
		free(tdata);
	perror("not enough memory: ");
	exit(-1);
}
