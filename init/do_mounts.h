/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/task_work.h>
#include <linux/file.h>

/* Ensure that async file closing finished to prevent spurious errors. */
static inline void init_flush_fput(void)
{
	flush_delayed_fput();
	task_work_run();
}
