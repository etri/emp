# mpi_test

> Generated with [Claude Code](https://claude.ai/claude-code)

MPI test programs for EMP module (user-level EMP with mimalloc-emp)

## Build

```
make all
```

## Run

```
make run-t1   # hello world
make run-t2   # malloc
make run-t3   # fork
make run-t4   # collective
make run-t5   # many ranks (np=16)
make run-t6   # pthread
make run-all  # all tests
```

## Tests

| File | Description |
|------|-------------|
| t1_hello.c | MPI hello world. Basic sanity check |
| t2_malloc.c | Each rank allocates a large buffer via malloc and verifies pattern |
| t3_fork.c | Each rank forks a child; child uses emp memory |
| t4_collective.c | Scatter / Allreduce / Gather with emp-backed buffers |
| t5_manyranks.c | 16 ranks simultaneously, stresses pidfd_getfd inheritance |
| t6_thread.c | 4 pthreads per rank, each thread allocates emp memory |

## Memory size

t2–t6 have `TOTAL_GB` at the top of each source file to control total memory usage across all ranks.

