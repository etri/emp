/*
 * t6_thread.c - Each MPI rank spawns multiple pthreads,
 *               each thread allocates and accesses emp memory.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define TOTAL_GB  32
#define BUF_SIZE  ((size_t)TOTAL_GB * 1024UL * 1024UL * 1024UL)
#define NTHREADS  4

struct thread_arg {
	int rank;
	int tid;
	int size;
	int result;
};

static void *worker(void *arg)
{
	struct thread_arg *a = arg;

	size_t per_thread = BUF_SIZE / (a->size * NTHREADS);

	char *buf = malloc(per_thread);
	if (!buf) {
		fprintf(stderr, "[rank %d thread %d] malloc failed\n", a->rank, a->tid);
		a->result = 0;
		return NULL;
	}

	unsigned char pattern = (a->rank * NTHREADS + a->tid) & 0xff;
	memset(buf, pattern, per_thread);

	a->result = 1;
	for (size_t i = 0; i < per_thread; i++) {
		if ((unsigned char)buf[i] != pattern) {
			a->result = 0;
			break;
		}
	}

	free(buf);
	return NULL;
}

int main(int argc, char *argv[])
{
	int provided;
	MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
	if (provided < MPI_THREAD_MULTIPLE)
		fprintf(stderr, "Warning: MPI_THREAD_MULTIPLE not fully supported\n");

	int rank, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	pthread_t threads[NTHREADS];
	struct thread_arg args[NTHREADS];

	for (int i = 0; i < NTHREADS; i++) {
		args[i].rank   = rank;
		args[i].tid    = i;
		args[i].size   = size;
		args[i].result = 0;
		pthread_create(&threads[i], NULL, worker, &args[i]);
	}

	int ok = 1;
	for (int i = 0; i < NTHREADS; i++) {
		pthread_join(threads[i], NULL);
		if (!args[i].result)
			ok = 0;
	}

	int all_ok;
	MPI_Reduce(&ok, &all_ok, 1, MPI_INT, MPI_MIN, 0, MPI_COMM_WORLD);

	if (rank == 0) {
		size_t per_thread = BUF_SIZE / (size * NTHREADS);
		printf("[rank 0/%d] %d threads x %zuMB per thread (total %dGB): %s\n",
		       size, NTHREADS, per_thread / (1024 * 1024), TOTAL_GB,
		       all_ok ? "OK" : "FAIL");
	}

	MPI_Finalize();
	return 0;
}
