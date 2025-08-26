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
//#include "____mmap.h"

#define LIBEMP_READY (empfd != -1)
int empfd = -1;

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

/**
 * libemp_init - Inaitalize Libemp. get EMP memory path, open EMP, and donate memory for malloc().
 */
void libemp_init(void) 
{
	int ret;
	char *param_emp_path;

#ifndef EMP_UNLOAD_DEBUG
	// get env for emp path
	param_emp_path = getenv("EMP_MEM_PATH");
	if (!param_emp_path) {
		fprintf(stderr, "%s: Invalid parameters\n", __func__);
		exit(-1);
	}

	// open emp 
	empfd = open("/dev/emp", O_RDWR);
	if (empfd < 0) {
		fprintf(stderr, "%s: Can not open /dev/emp. %s\n", __func__, strerror(errno));
		exit(-1);
	}

	// setup emp
	ret = emp_setup_media(empfd, param_emp_path);
	if (ret < 0) {
		fprintf(stderr, "%s: Invalid arguments: %s\n", __func__, param_emp_path);
		close(empfd);
		exit(-1);
	}
	fprintf(stderr, "%s: EMP open success - fd = %d\n", __func__, empfd);

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
