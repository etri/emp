/*
 * t2_malloc.c - Each rank allocates a large buffer via malloc,
 *               writes a pattern, and verifies it.
 *               This triggers emp page faults on first access.
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

	size_t per_rank = BUF_SIZE / size;

	char *buf = malloc(per_rank);
	if (!buf) {
		fprintf(stderr, "[rank %d] malloc failed\n", rank);
		MPI_Abort(MPI_COMM_WORLD, 1);
	}

	memset(buf, rank & 0xff, per_rank);

	int ok = 1;
	for (size_t i = 0; i < per_rank; i++) {
		if ((unsigned char)buf[i] != (rank & 0xff)) {
			fprintf(stderr, "[rank %d] mismatch at %zu\n", rank, i);
			ok = 0;
			break;
		}
	}

	printf("[rank %d/%d] malloc %zuMB (total %dGB): %s\n",
	       rank, size, per_rank / (1024 * 1024), TOTAL_GB, ok ? "OK" : "FAIL");

	free(buf);
	MPI_Finalize();
	return ok ? 0 : 1;
}
