/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PID_FS_H
#define _LINUX_PID_FS_H

#include <uapi/linux/pidfd.h>

struct file *pidfs_alloc_file(struct pid *pid, unsigned int flags);
void __init pidfs_init(void);

static inline int pidfd_validate_flags(unsigned int flags)
{
	if (flags & ~(PIDFD_NONBLOCK | PIDFD_THREAD))
		return -EINVAL;
	return 0;
}

#endif /* _LINUX_PID_FS_H */
