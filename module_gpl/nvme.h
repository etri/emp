#ifdef CONFIG_EMP_BLOCKDEV
#ifndef _NVME_H
#define _NVME_H

int nvme_open(void *);
void nvme_release(void *);
int nvme_init(void);
void nvme_exit(void);

#endif /* _NVME_H */
#endif
