#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "emp/emp_ioctl.h"
#include "emp/env.h"
#include "emp/libemp.h"

int empfd = -1;

int verbose = 0;
int emp_enabled = 0;

/**
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

/**
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

	if (param_emp_path[0] == '!') {
                // empfd was opened at parent process
                empfd = atoi(param_emp_path + 1);
                if (fdopen(empfd, "rw") == NULL)  {
                        fprintf(stderr, "libemp.so: [ERROR] Failed to inherit emp module: %s\n", strerror(errno));
                        exit(-1);
                }

                print_verbose("libemp.so: [INFO] succeed to inherit emp module. emp_fd: %d\n", empfd);

                emp_enable();
                return;
        }

	// open emp 
	empfd = open("/dev/emp", O_RDWR);
	if (empfd < 0) {
		fprintf(stderr, "libemp.so: [ERROR] Failed to open emp module: %s\n", strerror(errno));
		exit(-1);
	}

	// setup emp
	ret = emp_setup_media(empfd, param_emp_path);
	if (ret < 0) {
		fprintf(stderr, "libemp.so: [ERROR] Invalid arguments: %s\n", param_emp_path);
		close(empfd);
		exit(-1);
	}
        snprintf(empfd_str, 16, "!%d", empfd);
        setenv("EMP_MEM_PATH", empfd_str, 1);

	emp_enable();

#ifdef EMP_DEBUG
	fprintf(stderr, "%s: EMP open success : fd = %d\n", __func__, empfd);
#endif
	
}

/**
 * libemp_exit - free donated memory for malloc() and close EMP
 */
void libemp_exit(void)
{
	if(LIBEMP_READY) {
		close(empfd);
	}
}
