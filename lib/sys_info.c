// SPDX-License-Identifier: GPL-2.0-only
#include <linux/sched/debug.h>
#include <linux/kernel.h>
#include <linux/ftrace.h>
#include <linux/console.h>
#include <linux/nmi.h>

void sys_show_info(unsigned long info_flag)
{
	if (info_flag & SYS_SHOW_TASK_INFO)
		show_state();

	if (info_flag & SYS_SHOW_MEM_INFO)
		show_mem();

	if (info_flag & SYS_SHOW_TIMER_INFO)
		sysrq_timer_list_show();

	if (info_flag & SYS_SHOW_LOCK_INFO)
		debug_show_all_locks();

	if (info_flag & SYS_SHOW_FTRACE_INFO)
		ftrace_dump(DUMP_ALL);

	if (info_flag & SYS_SHOW_ALL_CPU_BT)
		trigger_all_cpu_backtrace();

	if (info_flag & SYS_SHOW_BLOCKED_TASKS)
		show_state_filter(TASK_UNINTERRUPTIBLE);
}
