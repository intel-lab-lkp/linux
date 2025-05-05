/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PID_FS_H
#define _LINUX_PID_FS_H

struct coredump_params;

struct file *pidfs_alloc_file(struct pid *pid, unsigned int flags);
void __init pidfs_init(void);
void pidfs_add_pid(struct pid *pid);
void pidfs_remove_pid(struct pid *pid);
void pidfs_exit(struct task_struct *tsk);
void pidfs_coredump(const struct coredump_params *cprm);
extern const struct dentry_operations pidfs_dentry_operations;
int pidfs_register_pid(struct pid *pid);
void pidfs_get_pid(struct pid *pid);
void pidfs_put_pid(struct pid *pid);
bool pidfs_pid_coredumped(const struct pid *pid);

#endif /* _LINUX_PID_FS_H */
