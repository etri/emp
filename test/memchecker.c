#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef unsigned long long u64;

//#define DEBUG
#ifdef DEBUG
typedef struct { long long val; } atomic64_t;
#define atomic64_set(var, value) __atomic_store_n(&((var).val), value, __ATOMIC_SEQ_CST)
#define atomic64_load(var) __atomic_load_n(&((var).val), __ATOMIC_SEQ_CST)
#define atomic64_inc(var) do { __atomic_add_fetch(&((var).val), 1, __ATOMIC_SEQ_CST); } while (0)
#endif

struct region {
	u64 *mem;
	u64 hash;
#ifdef DEBUG
	atomic64_t num_acc;
#endif
} *regions;

unsigned int num_region;
unsigned int num_thread;
unsigned int seed;
unsigned long region_size;
unsigned long stride; // multiple of sizeof(u64)
unsigned long num_rand_acc; // number of random access per thread

enum mode {
	MODE_SEQ,
	MODE_RAND,
	MODE_RAND_RW,
	MODE_ELASTIC // test correctness of elastic block management
} mode;

const char *mode_str[] = {
	"seq",
	"rand",
	"rand_rw",
	"elastic"
};

enum alloc_mode {
	ALLOC_MALLOC,
	ALLOC_MMAP
} alloc_mode;

enum multi_mode {
	MULTI_PTHREAD,
	MULTI_FORK
} multi_mode;

enum share_mode {
	SHARE_SHARED,
	SHARE_PRIVATE
} share_mode;

#ifdef DEBUG
#define debug_num_acc_init(region) atomic64_set((region)->num_acc, 0)
#define debug_num_acc_load(region) atomic64_load((region)->num_acc)
#define debug_num_acc_inc(region) atomic64_inc((region)->num_acc)
static void debug_num_acc_show(enum mode m) {
	int r;
	long long total_acc = 0, total_ref = 0;

	for (r = 0; r < num_region; r++) {
		struct region *region = &regions[r];
		long long acc = debug_num_acc_load(region);
		total_acc += acc;
		printf("DEBUG: region: %4d hash: %016llx num_access: %#16llx\n",
				r, region->hash, acc);
		debug_num_acc_init(region);
	}

	switch (m) {
	case MOST_SEQ:
	case MOST_ELASTIC:
		total_ref = num_region * region_size / stride;
		break;
	case MODE_RAND:
	case MODE_RAND_RW:
		total_ref = num_rand_acc * num_thread;
		break;
	}
	
	printf("DEBUG: #region: %d #thread: %d region_size: %#lx stride: %#lx num_rand_acc: %#lx\n",
			num_region, num_thread, region_size, stride, num_rand_acc);
	printf("DEBUG: mode: %s total_acc: %#llx total_ref: %#llx diff(ref-acc): %#llx\n", 
			mode_str[m], total_acc, total_ref, total_ref - total_acc);
}
#define dprintf(fmt, ...) printf(fmt, ##__VA_ARGS__);
#else
#define debug_num_acc_init(region) do {} while (0)
#define debug_num_acc_load(region) do {} while (0)
#define debug_num_acc_inc(region) do {} while (0)
#define debug_num_acc_show(m) do {} while (0)
#define dprintf(fmt, ...) do {} while (0)
#endif

#define ACC_DIST_MAX (100)
struct acc_dist {
	unsigned long start; // bytes
	unsigned long size; // bytes
} acc_dist[ACC_DIST_MAX];

static inline u64
gen_value(struct region *region, unsigned long idx, unsigned int mark) {
	u64 val = region->hash + idx;
	val ^= (((u64) mark) << 32) | (((u64) (~mark)) & 0xffffffffULL);
	return val;
}

static inline int
check_value(struct region *region, unsigned long idx) {
	u64 v = region->mem[idx];
	u64 val = v ^ (region->hash + idx);
	return ((val >> 32) == ((~val) & 0xffffffff)
			|| (v == 0 && share_mode == SHARE_PRIVATE))
				? 0 : -1;
}	

static inline double
timeval_diff(struct timeval *beg, struct timeval *end) {
	return ((double) (end->tv_sec * 10e6 + end->tv_usec
		- beg->tv_sec * 10e6 - beg->tv_usec)) / 10e6;
}

static inline unsigned int rand_32bit() {
	return ((rand() & 0xff) << 24)
		| ((rand() & 0xff) << 16)
		| ((rand() & 0xff) << 8)
		| (rand() & 0xff);
}

static inline u64 rand_64bit() {
	return (((u64) rand_32bit()) << 32) | ((u64) rand_32bit());
}

static inline void update_seed() {
	seed = rand_32bit();
}

static inline void init_acc_dist() {
	int i;
	unsigned long end = num_region * region_size;
	dprintf("acc_dist INIT end: %#lx num_region: %d region_size: %#lx stride: %ld\n", end, num_region, region_size);
	for (i = 0; i < ACC_DIST_MAX; i++) {
		struct acc_dist *a = &acc_dist[i];
		a->start = 0;
		a->size = end;
		dprintf("acc_dist[%02d]: %#18lx - %#18lx size: %#18lx\n",
				i, a->start, a->start + a->size, a->size);
	}
}

#define acc_dist_last (acc_dist[ACC_DIST_MAX - 1])
static inline int add_acc_dist(unsigned long size, int prob) {
	int i;
	unsigned long end = size / stride;
	dprintf("acc_dist ADD size: %#18lx prob: %d\n", size, prob);
	prob--; /* range of prob: 1~100, range of acc_dist: 0~99 */

	if (size > acc_dist_last.start + acc_dist_last.size)
		return -1; // too large size

	if (prob < 1 || prob > 100)
		return -2; // out-of-range for probability

	if (size % stride != 0)
		return -3;

	if (size > acc_dist[prob].start + acc_dist[prob].size)
		return -4;
	
	if (size <= acc_dist[prob].start)
		return -5;
	
	if (size == acc_dist[prob].start + acc_dist[prob].size)
		return 0;

	for (i = 0; i < ACC_DIST_MAX; i++) {
		struct acc_dist *a = &acc_dist[i];
		if (i <= prob) {
			if (a->start + a->size > size)
				a->size = size - a->start;
			// else, nothing to do
		} else {
			if (a->start < size) {
				a->size -= size - a->start;
				a->start = size;
			} else
				return -11;
		}

		dprintf("acc_dist[%02d]: %#18lx - %#18lx size: %#18lx\n",
				i, a->start, a->start + a->size, a->size);
	}

	return 0;
}

uint64_t fast_rand_64bit(uint64_t *state) {
	// wyhash64
	// refer to https://lemire.me/blog/2019/03/19/the-fastest-conventional-random-number-generator-that-can-pass-big-crush/
	__uint128_t tmp;
	uint64_t m1, m2;
	*state += 0x60bee2bee120fc15;
	tmp = (__uint128_t) (*state) * 0xa3b195354a39b70d;
	m1 = (tmp >> 64) ^ tmp;
	tmp = (__uint128_t)m1 * 0x1b03738712fad5c9;
	m2 = (tmp >> 64) ^ tmp;
	return m2;
}

void __sequential_write(struct region *region, unsigned int mark) {
	unsigned long i;
	unsigned long end = region_size / sizeof(u64);
	unsigned long step = stride / sizeof(u64);
	for (i = 0; i < end; i += step) {
		region->mem[i] = gen_value(region, i, mark);
		debug_num_acc_inc(region);
	}
}

void __sequential_read(struct region *region, unsigned int mark) {
	unsigned long i;
	unsigned long end = region_size / sizeof(u64);
	unsigned long step = stride / sizeof(u64);
	u64 val;
	for (i = 0; i < end; i += step) {
		debug_num_acc_inc(region);
		val = region->mem[i];
		if (val == gen_value(region, i, mark)
			|| (val == 0 && share_mode == SHARE_PRIVATE))
			continue;
		fprintf(stderr, "ERROR: (sequantial_read) integrity breaks. region: 0x%lx index: 0x%lx hash: 0x%llx mark: 0x%x value: 0x%llx\n",
					region - regions, i, region->hash, mark, region->mem[i]);
		break;
	}
}

void *sequential_write(void *data) {
	int tid = (int) (unsigned long) data;
	int task_id = (seed + tid) % num_thread;
	struct region *region;
	for (task_id = (seed + tid) % num_thread;
			task_id < num_region;
			task_id += num_thread) {
		__sequential_write(&regions[task_id], tid);
	}
	pthread_exit(NULL);
}

void *sequential_read(void *data) {
	int tid = (int) (unsigned long) data;
	int task_id = (seed + tid) % num_thread;
	struct region *region;
	for (task_id = (seed + tid) % num_thread;
			task_id < num_region;
			task_id += num_thread) {
		__sequential_read(&regions[task_id], tid);
	}
	pthread_exit(NULL);
}

void *random_write(void *data) {
	int tid = (int) (unsigned long) data;
	uint64_t rand_state = rand_64bit();
	struct region *region;
	int p, r;
	u64 addr;
	unsigned long i, num = num_rand_acc;
	int mark = tid;
	unsigned long stride_u64 = stride / sizeof(u64);

	for (i = 0; i < num; i++) {
		p = fast_rand_64bit(&rand_state) % 100;
		addr = acc_dist[p].start + (fast_rand_64bit(&rand_state) % acc_dist[p].size);
		r = (seed + (addr / region_size)) % num_region;
		addr = (addr % region_size) / sizeof(u64);
		addr = addr - (addr % stride_u64);
		region = &regions[r];
		debug_num_acc_inc(region);
		if (check_value(region, addr)) {
			fprintf(stderr, "ERROR: (random_write) integrity breaks. region: 0x%lx index: 0x%llx hash: 0x%llx value: 0x%llx\n",
						region - regions, addr, region->hash, region->mem[addr]);
		}
		region->mem[addr] = gen_value(region, addr, mark);
		mark += num_thread;
	}
	pthread_exit(NULL);
}

void *random_read(void *data) {
	int tid = (int) (unsigned long) data;
	uint64_t rand_state = rand_64bit();
	struct region *region;
	int p, r;
	u64 addr;
	unsigned long i, num = num_rand_acc;
	unsigned long stride_u64 = stride / sizeof(u64);

	for (i = 0; i < num; i++) {
		p = fast_rand_64bit(&rand_state) % 100;
		addr = acc_dist[p].start + (fast_rand_64bit(&rand_state) % acc_dist[p].size);
		r = (seed + (addr / region_size)) % num_region;
		addr = (addr % region_size) / sizeof(u64);
		addr = addr - (addr % stride_u64);
 		region = &regions[r];
		debug_num_acc_inc(region);
		if (check_value(region, addr)) {
			fprintf(stderr, "ERROR: (random_read) integrity breaks. region: 0x%lx index: 0x%llx hash: 0x%llx value: 0x%llx\n",
						region - regions, addr, region->hash, region->mem[addr]);
		}
	}
	pthread_exit(NULL);
}

int __do_pthreads(pthread_t *threads, int num, void *(*func)(void *data)) {
	unsigned long i;
	int tid;
	int status;
	for (i = 0; i < num; i++) {
		tid = pthread_create(&threads[i], NULL, func, (void *) i);
		if (tid < 0) {
			fprintf(stderr, "ERROR: failed to launch thread. errno: %d\n", tid);
			return -1;
		}
	}

	for (i = 0; i < num_thread; i++) {
		pthread_join(threads[i], (void **)&status);
		memset(&threads[i], 0, sizeof(pthread_t));
	}
}

int __do_fork(pid_t *threads, int num, void *(*func)(void *data)) {
	unsigned long i;
	int pid;
	int status;
	for (i = 0; i < num; i++) {
		pid = fork();
		if (pid == 0) {
			func((void *)i);
			exit(EXIT_SUCCESS);
		} else if (pid > 0) {
			threads[i] = pid;
		} else if (pid < 0) {
			fprintf(stderr, "ERROR: failed to fork thread. errno: %d\n", errno);
			return -1;
		}
	}

	for (i = 0; i < num_thread; i++) {
		waitpid(threads[i], &status, 0);
		threads[i] = 0;
	}
}

int do_threads(void *threads, int num, void *(*func)(void *data)) {
	struct timeval beg, end;
	if (multi_mode == MULTI_PTHREAD)
		memset(threads, 0, num * sizeof(pthread_t));
	else // multi_mode == MULTI_FORK
		memset(threads, 0, num * sizeof(pid_t));
	gettimeofday(&beg, NULL);
	if (multi_mode == MULTI_PTHREAD)
		__do_pthreads((pthread_t *) threads, num, func);
	else // multi_mode == MULTI_FORK
		__do_fork((pid_t *) threads, num, func);
	gettimeofday(&end, NULL);
	printf("Time: %.3f\n", timeval_diff(&beg, &end));
	return 0;
}

int integrity_check() {
	int ret = 0;
	int r;
	unsigned long i;
	int mmap_flags;
	u64 hash;
	void *threads = NULL;

	printf("%s: num_thread: %d num_region: %d region_size: %.3fGB\n",
			__func__, num_thread, num_region,
			((double) region_size) / (1UL << 30));

	threads = calloc(num_thread, multi_mode == MULTI_PTHREAD
						? sizeof(pthread_t)
						: sizeof(pid_t));
	if (threads == NULL) {
		fprintf(stderr, "ERROR: failed to allocate memory\n");
		ret = -1;
		goto out;
	}

	regions = (struct region *) calloc(num_region, sizeof(struct region));
	if (regions == NULL) {
		fprintf(stderr, "ERROR: failed to allocate memory\n");
		ret = -1;
		goto out;
	}

	if (alloc_mode == ALLOC_MMAP) {
		mmap_flags = MAP_ANONYMOUS;
		if (share_mode == SHARE_SHARED)
			mmap_flags |= MAP_SHARED;
		else
			mmap_flags |= MAP_PRIVATE;
	}

	for (r = 0; r < num_region; r++) {
		if (alloc_mode == ALLOC_MMAP) {
			regions[r].mem = mmap(NULL, region_size,
						PROT_READ|PROT_WRITE,
						mmap_flags, -1, 0);
			if (regions[r].mem == MAP_FAILED) {
				fprintf(stderr, "ERROR: failed to allocate memory\n");
				ret = -1;
				goto out;
			}
		} else { // alloc_mode == ALLOC_MALLOC
			regions[r].mem = malloc(region_size);
			if (regions[r].mem == NULL) {
				fprintf(stderr, "ERROR: failed to allocate memory\n");
				ret = -1;
				goto out;
			}
		}

		regions[r].hash = rand_64bit();
		debug_num_acc_init(&regions[r]);
	}
	
	update_seed();
	if (mode == MODE_SEQ) {
		printf("Step1: write values sequentially\n");
		do_threads(threads, num_thread, sequential_write);
		debug_num_acc_show(MODE_SEQ);
		printf("Step2: check the values sequentially\n");
		do_threads(threads, num_thread, sequential_read);
		debug_num_acc_show(MODE_SEQ);
	} else if (mode == MODE_RAND) {
		printf("Step1: write values sequentially\n");
		do_threads(threads, num_thread, sequential_write);
		debug_num_acc_show(MODE_SEQ);
		update_seed();
		printf("Step2: check the values randomly\n");
		do_threads(threads, num_thread, random_read);
		debug_num_acc_show(MODE_RAND);
	} else if (mode == MODE_RAND_RW) {
		printf("Step1: write values sequentially\n");
		do_threads(threads, num_thread, sequential_write);
		debug_num_acc_show(MODE_SEQ);
		update_seed();
		printf("Step2: check and write the values randomly\n");
		do_threads(threads, num_thread, random_write);
		debug_num_acc_show(MODE_RAND_RW);
	} else if (mode == MODE_ELASTIC) {
		int step = 1;
		int stride_for_step[] = {0, 0, // step 0, 1: not used
				4096, 4096, 4096,
				1 << 20, 1 << 20, 1 << 20,
				4096, 4096, 4096,
				1 << 20, 1 << 20, 1 << 20,
				-1};
		printf("Step1: write values sequentially\n");
		do_threads(threads, num_thread, sequential_write);
		debug_num_acc_show(MODE_SEQ);
		while (stride_for_step[++step] >= 0) {
			stride = stride_for_step[step];
			if (stride == 0)
				continue;
			printf("Step%d: check the values with stride: %ld\n",
					step, stride);
			do_threads(threads, num_thread, sequential_read);
			debug_num_acc_show(MODE_SEQ);
		}
	} else
		fprintf(stderr, "ERROR: wrong mode\n");

out:
	if (threads) {
		int status;
		if (multi_mode == MULTI_PTHREAD) {
			pthread_t *th = (pthread_t *) threads;
			pthread_t zero;
			memset(&zero, 0, sizeof(pthread_t));
			for (i = 0; i < num_thread; i++) {
				if (memcmp(&th[i], &zero, sizeof(pthread_t)))
					pthread_join(th[i], (void **)&status);
			}
		} else { // multi_mode == MULTI_FORK
			pid_t *th = (pid_t *) threads;
			for (i = 0; i < num_thread; i++) {
				if (th[i] > 0)
					waitpid(th[i], &status, 0);
			}
		}
		free(threads);
	}

	if (regions) {
		if (alloc_mode == ALLOC_MMAP) {
			for (r = 0; r < num_region; r++) {
				if (regions[r].mem)
					munmap(regions[r].mem, region_size);
			}
		} else { // alloc_mode == ALLOC_MALLOC
			for (r = 0; r < num_region; r++) {
				if (regions[r].mem)
					free(regions[r].mem);
			}
		}
		free(regions);
	}
	return -1;
}

void usage(char *argv0) {
	fprintf(stderr, "usage: %s -t [#thread] -s [size(GB)] -m [mode] -n [#random access] -d [distribution] -r [stride size]\n"
			"       -t number of threads. [default: number of processors]\n"
			"       -s total memory size in GB. [default: 16]\n"
			"       -m mode: seq, rand, rand_rw [default: seq]\n"
			"                seq: sequential_write + sequential_read\n"
			"                rand: sequential_write + random_read\n"
			"                rand_rw: sequential_write + random_read_write\n"
			"                elastic: sequential_write + sequantial_read with varying stide.\n"
			"       -n number of accesses for each thread. \n"
			"          only for random_read and random_read_write. [default: 1000000000]\n"
			"       -d define distribution of accesses: size_in_bytes,percent\n"
			"          this option can be given multiple times.\n"
			"          e.g. -d 4096,50 => 50%% of accesses are on the first 4KB\n"
			"          e.g. -d $((1 << 30),90 => 90%% of accesses are on the first 1GB\n"
			"       -r stide size in bytes. [default: 8]\n"
			"       -a allocation function: mmap or malloc [default: malloc]\n"
			"       -p multi-process methodology: pthread or fork [default: pthread]\n"
			"       -i memory sharing: shared or private [default: shared]\n"
			"          this option works only when allocation function is mmap.\n",
			argv0);
}

struct param_acc_dist {
	size_t size;
	int prob;
	struct param_acc_dist *next;
};

static int insert_dist_list(struct param_acc_dist **list, const char *argv) {
	char *ptr, *endptr;
	size_t size;
	int prob;
	struct param_acc_dist *new = NULL, *prev, *curr;

	size = strtol(argv, &endptr, 0);
	if (argv == endptr || *endptr != ',')
		goto str_error;

	ptr = endptr + 1;
	prob = (int) strtol(ptr, &endptr, 0);
	if (ptr == endptr || *endptr != '\0')
		goto str_error;
	
	dprintf("DEBUG: %s size: %s -> %#lx prob: %s -> %d\n", __func__, argv, size, ptr, prob);

	new = malloc(sizeof(struct param_acc_dist));
	if (new == NULL) {
		fprintf(stderr, "ERROR: failed to allocate memory for dist_list\n");
		return -2;
	}

	new->size = size;
	new->prob = prob;
	new->next = NULL;

	prev = NULL;
	curr = *list;
	while (curr != NULL) {
		if (curr->prob < prob) {
			if (curr->size > size)
				goto size_error;
			prev = curr;
		} else if (curr->prob > prob) {
			if (curr->size < size)
				goto size_error;
		} else { // curr->prob == prob
			fprintf(stderr, "ERROR: access distribution for probability %d%% is given twice.\n", prob);
			return -3;
		}
		curr = curr->next;
	}

	if (prev == NULL) {
		new->next = *list;
		*list = new;
	} else {
		new->next = prev->next;
		prev->next = new;
	}

	return 0;

str_error:
	fprintf(stderr, "ERROR: \"%s\" is not a valid string for access distribution\n", argv);
	return -1;

size_error:
	fprintf(stderr, "ERROR: access distribution should be monotonically increasing.\n");
	if (new) free(new);
	return -4;
}

int main(int argc, char **argv) {
	int opt;	
	struct param_acc_dist *dist_head = NULL, *dist_prev = NULL, *dist_curr = NULL;

	/* default values */
	num_thread = (int) sysconf(_SC_NPROCESSORS_ONLN); 
	mode = MODE_SEQ;
	region_size = 1UL << 30;
	num_region = 16;
	num_rand_acc = 1000000000UL;
	stride = 8;
	alloc_mode = ALLOC_MALLOC;
	multi_mode = MULTI_PTHREAD;
	share_mode = SHARE_SHARED;

	while ((opt = getopt(argc, argv, "t:s:m:n:d:r:a:p:i:")) != -1) {
		switch(opt) {
		case 't':
			num_thread = atoi(optarg);
			if (num_thread == 0)
				num_thread = (int) sysconf(_SC_NPROCESSORS_ONLN); 
			break;
		case 's':
			num_region = atoi(optarg);
			break;
		case 'm':
			if (strcmp(optarg, "seq") == 0)
				mode = MODE_SEQ;
			else if (strcmp(optarg, "rand") == 0)
				mode = MODE_RAND;
			else if (strcmp(optarg, "rand_rw") == 0)
				mode = MODE_RAND_RW;
			else if (strcmp(optarg, "elastic") == 0)
				mode = MODE_ELASTIC;
			else {
				fprintf(stderr, "ERROR: mode should be the one of seq, rand, rand_rw\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'n':
			num_rand_acc = strtol(optarg, NULL, 0);
			break;
		case 'd':
			if (insert_dist_list(&dist_head, optarg)) {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'r':
			stride = strtol(optarg, NULL, 0);
			if (stride % sizeof(u64) > 0) {
				fprintf(stderr, "ERROR: stride(0x%lx) should be the multiple of 8.\n", stride);
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'a':
			if (strcmp(optarg, "mmap") == 0)
				alloc_mode = ALLOC_MMAP;
			else if (strcmp(optarg, "malloc") == 0)
				alloc_mode = ALLOC_MALLOC;
			else {
				fprintf(stderr, "ERROR: allocation function should be the one of mmap or malloc\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'p':
			if (strcmp(optarg, "pthread") == 0)
				multi_mode = MULTI_PTHREAD;
			else if (strcmp(optarg, "fork") == 0)
				multi_mode = MULTI_FORK;
			else {
				fprintf(stderr, "ERROR: multi-process methodology should be the one of pthread or fork\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'i':
			if (strcmp(optarg, "shared") == 0)
				share_mode = SHARE_SHARED;
			else if (strcmp(optarg, "private") == 0)
				share_mode = SHARE_PRIVATE;
			else {
				fprintf(stderr, "ERROR: memory sharing option should be the one of shared or private\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
			break;
		default:
			fprintf(stderr, "ERROR: unknown option: %c\n", (char) opt);
			usage(argv[0]);
			exit(EXIT_FAILURE);
			break;
		}
	}

	if (mode == MODE_ELASTIC) {
		region_size = region_size * num_region;
		num_region = 1;
		// if num_thread > 1, the following loop will adjust the number of regions
	}

	/* adjust the values */
	while (num_region < num_thread) {
		region_size /= 2;
		num_region *= 2;
	}

	if (region_size == 0) {
		fprintf(stderr, "ERROR: too small size\n");
		return -1;
	}
	
	init_acc_dist();
	dist_curr = dist_head;
	while (dist_curr) {
		add_acc_dist(dist_curr->size, dist_curr->prob);
		dist_prev = dist_curr;
		dist_curr = dist_curr->next;
		free(dist_prev);
	}

	return integrity_check();
}
