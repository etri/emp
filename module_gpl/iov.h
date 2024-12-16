#ifndef __IOV_H__
#define __IOV_H__

#ifdef CONFIG_EMP_IO
/* struct QEMUIOVector came from qemu-common.h */
struct QEMUIOVector {
	struct iovec __user *iov;
	int niov;
	int nalloc;
	size_t size;
};

void handle_qiov(struct emp_mm *, bool, struct iovec *, int);
#endif

#endif /* __IOV_H__ */
