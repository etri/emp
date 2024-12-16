#ifdef CONFIG_EMP_MEMDEV
#ifndef _MEMDEV_H
#define _MEMDEV_H

int memdev_open(void *);
void memdev_release(void *);
int memdev_init(void);
void memdev_exit(void);

#endif /* _MEMDEV_H */
#endif /* CONFIG_EMP_MEMDEV */
