/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/mount.h>
#include <linux/major.h>
#include <linux/root_dev.h>
#include <linux/init_syscalls.h>
#include <linux/task_work.h>
#include <linux/file.h>

void  mount_root_generic(char *name, char *pretty_name, int flags);
void  mount_root(char *root_device_name);
extern int root_mountflags;

/* Ensure that async file closing finished to prevent spurious errors. */
static inline void init_flush_fput(void)
{
	flush_delayed_fput();
	task_work_run();
}
