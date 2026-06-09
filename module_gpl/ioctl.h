#ifndef __IOCTL_H__
#define __IOCTL_H__
long emp_unlocked_ioctl(struct file *file, unsigned int ioctl_num,
		unsigned long ioctl_param);
#endif /* __IOCTL_H__ */
