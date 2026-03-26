/*
 * t4_collective.c - MPI collective ops (Scatter/Gather/Allreduce)
 *                   with emp-backed buffers.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOTAL_GB  32
#define BUF_SIZE  ((size_t)TOTAL_GB * 1024UL * 1024UL * 1024UL)

int main(int argc, char *argv[])
{
	MPI_Init(&argc, &argv);

	int rank, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	/* N: number of longs per rank so total recvbuf = TOTAL_GB */
	size_t N = BUF_SIZE / size / sizeof(long);

	long *sendbuf = NULL;
	if (rank == 0) {
		sendbuf = malloc(N * size * sizeof(long));
		if (!sendbuf) {
			fprintf(stderr, "[rank 0] malloc sendbuf failed\n");
			MPI_Abort(MPI_COMM_WORLD, 1);
		}
		for (size_t i = 0; i < N * size; i++)
			sendbuf[i] = i;
	}

	long *recvbuf = malloc(N * sizeof(long));
	if (!recvbuf) {
		fprintf(stderr, "[rank %d] malloc recvbuf failed\n", rank);
		MPI_Abort(MPI_COMM_WORLD, 1);
	}

	MPI_Scatter(sendbuf, N, MPI_LONG,
	            recvbuf, N, MPI_LONG,
	            0, MPI_COMM_WORLD);

	for (size_t i = 0; i < N; i++)
		recvbuf[i] *= (rank + 1);

	long *redbuf = malloc(N * sizeof(long));
	if (!redbuf) {
		fprintf(stderr, "[rank %d] malloc redbuf failed\n", rank);
		MPI_Abort(MPI_COMM_WORLD, 1);
	}
	MPI_Allreduce(recvbuf, redbuf, N, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);

	long *gatherbuf = NULL;
	if (rank == 0) {
		gatherbuf = malloc(N * size * sizeof(long));
		if (!gatherbuf) {
			fprintf(stderr, "[rank 0] malloc gatherbuf failed\n");
			MPI_Abort(MPI_COMM_WORLD, 1);
		}
	}
	MPI_Gather(redbuf, N, MPI_LONG,
	           gatherbuf, N, MPI_LONG,
	           0, MPI_COMM_WORLD);

	if (rank == 0) {
		printf("[rank 0/%d] Scatter/Allreduce/Gather total %dGB: OK\n",
		       size, TOTAL_GB);
		free(gatherbuf);
		free(sendbuf);
	}

	free(redbuf);
	free(recvbuf);
	MPI_Finalize();
	return 0;
}
