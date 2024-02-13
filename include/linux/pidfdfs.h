/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PIDFDFS_H
#define _LINUX_PIDFDFS_H

struct file *pidfdfs_alloc_file(struct pid *pid, unsigned int flags);
void __init pidfdfs_init(void);
void pid_init_pidfdfs(struct pid *pid);

#endif /* _LINUX_PIDFDFS_H */
