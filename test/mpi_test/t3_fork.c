/*
 * t3_fork.c - Each MPI rank forks a child process.
 *             Child allocates memory and verifies empfd is usable.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define TOTAL_GB  32
#define BUF_SIZE  ((size_t)TOTAL_GB * 1024UL * 1024UL * 1024UL)

int main(int argc, char *argv[])
{
	MPI_Init(&argc, &argv);

	int rank, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	size_t per_rank = BUF_SIZE / size;

	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "[rank %d] fork failed\n", rank);
		MPI_Abort(MPI_COMM_WORLD, 1);
	}

	if (pid == 0) {
		/* child */
		char *buf = malloc(per_rank);
		if (!buf) {
			fprintf(stderr, "[rank %d child] malloc failed\n", rank);
			exit(1);
		}
		memset(buf, 0xab, per_rank);
		int ok = 1;
		for (size_t i = 0; i < per_rank; i++) {
			if ((unsigned char)buf[i] != 0xab) {
				ok = 0;
				break;
			}
		}
		printf("[rank %d/%d child pid=%d] malloc %zuMB (total %dGB): %s\n",
		       rank, size, getpid(), per_rank / (1024 * 1024), TOTAL_GB, ok ? "OK" : "FAIL");
		free(buf);
		exit(ok ? 0 : 1);
	}

	int status;
	waitpid(pid, &status, 0);
	printf("[rank %d/%d] child exited with %d\n",
	       rank, size, WEXITSTATUS(status));

	MPI_Finalize();
	return 0;
}
