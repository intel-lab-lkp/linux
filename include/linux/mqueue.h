/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_MQUEUE_H
#define __LINUX_MQUEUE_H

#include <uapi/linux/mqueue.h>

struct file;

#ifdef CONFIG_POSIX_MQUEUE
long do_mq_peek(struct file *filp, struct mq_peek_attr __user *uattr);
#else
static inline long do_mq_peek(struct file *filp,
			       struct mq_peek_attr __user *uattr)
{
	return -EBADF;
}
#endif /* CONFIG_POSIX_MQUEUE */

#endif /* __LINUX_MQUEUE_H */
