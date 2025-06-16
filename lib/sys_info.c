// SPDX-License-Identifier: GPL-2.0-only
#include <linux/sched/debug.h>
#include <linux/kernel.h>
#include <linux/ftrace.h>
#include <linux/console.h>
#include <linux/nmi.h>

struct sys_info_name {
	unsigned long bit;
	const char *name;
};

static const char sys_info_avail[] = "tasks,mem,timer,lock,ftrace,all_bt,blocked_tasks";

static const struct sys_info_name  si_names[] = {
	{ SYS_SHOW_TASK_INFO,	"tasks" },
	{ SYS_SHOW_MEM_INFO,	"mem" },
	{ SYS_SHOW_TIMER_INFO,	"timer" },
	{ SYS_SHOW_LOCK_INFO,	"lock" },
	{ SYS_SHOW_FTRACE_INFO, "ftrace" },
	{ SYS_SHOW_ALL_CPU_BT,	"all_bt" },
	{ SYS_SHOW_BLOCKED_TASKS, "blocked_tasks" },
};

/* Expecting string like "xxx_sys_info=tasks,mem,timer,lock,ftrace,..." */
unsigned long sys_info_parse_param(char *str)
{
	unsigned long si_bits = 0;
	char *s, *name;
	int i;

	s = str;
	while ((name = strsep(&s, ",")) && *name) {
		for (i = 0; i < ARRAY_SIZE(si_names); i++) {
			if (!strcmp(name, si_names[i].name)) {
				si_bits |= si_names[i].bit;
				break;
			}
		}
	}

	return si_bits;
}

#ifdef CONFIG_SYSCTL
int sysctl_sys_info_handler(const struct ctl_table *ro_table, int write,
					  void *buffer, size_t *lenp,
					  loff_t *ppos)
{
	char names[sizeof(sys_info_avail) + 1];
	struct ctl_table table;
	unsigned long *si_bits_global;
	int i, ret, len;

	si_bits_global = ro_table->data;

	if (write) {
		unsigned long si_bits;

		table = *ro_table;
		table.data = names;
		table.maxlen = sizeof(names);
		ret = proc_dostring(&table, write, buffer, lenp, ppos);
		if (ret)
			return ret;

		si_bits = sys_info_parse_param(names);
		/*
		 * The access to the global value is not synchronized.
		 */
		WRITE_ONCE(*si_bits_global, si_bits);
		return 0;
	} else {
		/* for 'read' operation */
		bool first = true;
		char *buf;

		buf = names;
		for (i = 0; i < ARRAY_SIZE(si_names); i++) {
			if (*si_bits_global & si_names[i].bit) {

				if (first) {
					first = false;
				} else {
					*buf = ',';
					buf++;
				}

				len = strlen(si_names[i].name);
				strncpy(buf, si_names[i].name, len);
				buf += len;
			}

		}
		*buf = '\0';

		table = *ro_table;
		table.data = names;
		table.maxlen = sizeof(names);
		return proc_dostring(&table, write, buffer, lenp, ppos);
	}
}
#endif

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
