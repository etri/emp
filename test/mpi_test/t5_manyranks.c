/*
 * t5_manyranks.c - Many ranks each inherit empfd and allocate memory.
 *                  Run with -np 16 or more to stress pidfd_getfd inheritance.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TOTAL_GB  32
#define BUF_SIZE  ((size_t)TOTAL_GB * 1024UL * 1024UL * 1024UL)

int main(int argc, char *argv[])
{
	MPI_Init(&argc, &argv);

	int rank, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	size_t per_rank = BUF_SIZE / size;

	char *buf = malloc(per_rank);
	if (!buf) {
		fprintf(stderr, "[rank %d] malloc failed\n", rank);
		MPI_Abort(MPI_COMM_WORLD, 1);
	}

	memset(buf, rank & 0xff, per_rank);

	MPI_Barrier(MPI_COMM_WORLD);

	int ok = 1;
	for (size_t i = 0; i < per_rank; i++) {
		if ((unsigned char)buf[i] != (rank & 0xff)) {
			ok = 0;
			break;
		}
	}

	int all_ok;
	MPI_Reduce(&ok, &all_ok, 1, MPI_INT, MPI_MIN, 0, MPI_COMM_WORLD);

	if (rank == 0)
		printf("[rank 0/%d] all ranks malloc %zuMB each (total %dGB): %s\n",
		       size, per_rank / (1024 * 1024), TOTAL_GB, all_ok ? "OK" : "FAIL");

	free(buf);
	MPI_Finalize();
	return 0;
}
