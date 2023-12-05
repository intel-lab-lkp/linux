/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_MEMFD_H
#define __LINUX_MEMFD_H

#include <linux/file.h>

#ifdef CONFIG_MEMFD_CREATE
extern long memfd_fcntl(struct file *file, unsigned int cmd, unsigned int arg);
extern struct page *memfd_alloc_page(struct file *memfd, pgoff_t idx);
#else
static inline long memfd_fcntl(struct file *f, unsigned int c, unsigned int a)
{
	return -EINVAL;
}
static inline struct page *memfd_alloc_page(struct file *memfd, pgoff_t idx)
{
	return ERR_PTR(-EINVAL);
}
#endif

#endif /* __LINUX_MEMFD_H */
