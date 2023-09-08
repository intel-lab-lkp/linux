/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_MEMFD_H
#define __LINUX_MEMFD_H

#include <linux/file.h>
#include <linux/ioctl.h>

#ifdef CONFIG_MEMFD_CREATE
extern long memfd_fcntl(struct file *file, unsigned int cmd, unsigned int arg);
extern long memfd_ioctl(struct file *file, unsigned int cmd, unsigned int arg);
#else
static inline long memfd_fcntl(struct file *f, unsigned int c, unsigned int a)
{
	return -EINVAL;
}
static inline long memfd_ioctl(struct file *f, unsigned int c, unsigned int a)
{
	return -EINVAL;
}
#endif

/*
 * Return 1 if the memfd is original (i.e. was created by memfd_create,
 * not reopened), 0 otherwise.
 */
#define MEMFD_CHECK_IF_ORIGINAL		_IOR(0xB8, 0, int)

#endif /* __LINUX_MEMFD_H */
