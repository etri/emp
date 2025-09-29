#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <limits.h>

#define __USE_GNU
#include <dlfcn.h>

#include <emp/emp_ioctl.h>
#include <emp/libemp.h>
#include <emp/env.h>

#define MADV_EMP_PINNING 30	// parameters of MADV_* is in linux [0,24]

#define CONTAINER_PID1	1	// PID 1 is the container root process

int empfd = -1;

int verbose = 0;
int emp_enabled = 0;

/*
 * get_empfd - return emp fd
 *
 * @retval -1: Error
 * @retval an emp descriptor: Success
 */
 int get_empfd(void)
{
	        return empfd;
}

void emp_disable(void)
{
	print_verbose("emp is disabled\n");
	emp_enabled = 0;
}

void emp_enable(void)
{
	print_verbose("emp is enabled\n");
	emp_enabled = 1;
}

int emp_save(void)
{
	int emp_status = emp_enabled;
	emp_disable();
	return emp_status;
}

void emp_restore(int emp_status)
{
	if (emp_status)
		emp_enable();
	else
		emp_disable();
}

// Find the file descriptor that points to /dev/emp from a specific PID
static int find_empfd_from_pid(pid_t pid)
{
    char path[PATH_MAX];
    DIR *dir;
    struct dirent *entry;

    // Get the device number of /dev/emp
    struct stat st_emp;
    if (stat("/dev/emp", &st_emp) < 0)
        return -1;

    snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    dir = opendir(path);
    if (!dir)
        return -1;

    int found_fd = -1;
    while ((entry = readdir(dir)) != NULL) {
		if (!('0' <= entry->d_name[0] && entry->d_name[0] <= '9'))
            continue;

        int fd = atoi(entry->d_name);
        if (fd <= 2)
            continue;

        // Get the actual target of the fd (follow symlink)
        snprintf(path, sizeof(path), "/proc/%d/fd/%d", pid, fd);
        struct stat st_target;
        if (stat(path, &st_target) < 0)
            continue;

        // If it's a character device and has the same rdev as /dev/emp, it's a match
        if (S_ISCHR(st_target.st_mode) && st_target.st_rdev == st_emp.st_rdev) {
            found_fd = fd;
            break;
        }
    }

    closedir(dir);
    return found_fd;
}

/*
 * libemp_init - Inaitalize Libemp. get EMP memory path, open EMP, and donate memory for malloc().
 */
void libemp_init(void) 
{
	char empfd_str[16];
	int ret;
	char *param_emp_path, *param_emp_verbose;

	// get env for emp path
	param_emp_path = getenv("EMP_MEM_PATH");
	if (!param_emp_path) {
		fprintf(stderr, "libemp.so: [ERROR] no emp_path parameters\n");
		exit(-1);
	}

	// get env for verbose
	param_emp_verbose = getenv("EMP_VERBOSE");
	if (param_emp_verbose)
		verbose = atoi(param_emp_verbose);

	// empfd was opened at parent process
	// Check the child processes of the legacy parent process
	if (param_emp_path[0] == '!') {
		empfd = atoi(param_emp_path + 1);
		if (fdopen(empfd, "rw") == NULL)  {
			fprintf(stderr, "libemp.so: [ERROR] Failed to inherit emp module: %s\n", strerror(errno));
			exit(-1);
		}

		print_verbose("libemp.so: [INFO] succeed to inherit emp module. emp_fd: %d\n", empfd);

		emp_enable();
		return;
	}

	// Check if the container process has initialized the emp module and inherit the empfd
	// Only check the child processes of the container root process
	pid_t ppid = getppid();
	if (param_emp_path[0] != '!' && ppid == CONTAINER_PID1) {
		int max_retries = 100;  // Maximum 100 retries (approx. 10 seconds)
		int retry_count = 0;

		while (retry_count < max_retries) {
			// Find the empfd of PID 1 (container root process)
			int pid1_empfd = find_empfd_from_pid(CONTAINER_PID1);
			if (pid1_empfd >= 0) {
				// PID 1 has the empfd
				// Check if the same number of file descriptors are inherited in the current process
				char empfd_path[PATH_MAX];
				snprintf(empfd_path, sizeof(empfd_path), "/proc/self/fd/%d", pid1_empfd);
				char link_target[PATH_MAX];
				ssize_t len = readlink(empfd_path, link_target, sizeof(link_target) - 1);

				if (len > 0) {
					link_target[len] = '\0';
					if (strcmp(link_target, "/dev/emp") == 0) {
						// Found the inherited empfd
						empfd = pid1_empfd;
						snprintf(empfd_str, 16, "!%d", empfd);
						setenv("EMP_MEM_PATH", empfd_str, 1);

						print_verbose("libemp.so: [INFO] succeed to inherit emp module from PID 1. emp_fd: %d\n", empfd);
						emp_enable();
						return;
					}
				}
			}

			// If PID 1 has not initialized the emp module or failed to inherit the empfd, wait and retry
			usleep(100000);  // Wait for 100ms (100ms * 100 retries = 10 seconds)
			retry_count++;
		}

		// Exceeded the retry count but failed to inherit the empfd from PID 1
		// In this case, do not open a new /dev/emp and call emp_setup_media (kernel panic will occur)
		fprintf(stderr, "libemp.so: [ERROR] Cannot inherit empfd from PID 1 after %d retries\n", max_retries);
		exit(-1);
	}

	// Parent process or did not find the empfd of PID 1, open a new /dev/emp
	empfd = open("/dev/emp", O_RDWR);
	if (empfd < 0) {
		fprintf(stderr, "libemp.so: [ERROR] Failed to open emp module: %s\n", strerror(errno));
		exit(-1);
	}

	ret = emp_setup_media(empfd, param_emp_path);
	if (ret < 0) {
		fprintf(stderr, "libemp.so: [ERROR] Invalid arguments: %s\n", param_emp_path);
		close(empfd);
		exit(-1);
	}

	print_verbose("libemp.so: [INFO] succeed to load emp module. emp_fd: %d\n", empfd);

	snprintf(empfd_str, 16, "!%d", empfd);
	setenv("EMP_MEM_PATH", empfd_str, 1);

	emp_enable();
}

/*
 * libemp_exit - free donated memory for malloc() and close EMP
 */
void libemp_exit(void)
{
	if (LIBEMP_READY)
		close(empfd);
}
