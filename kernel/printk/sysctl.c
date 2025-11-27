// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysctl.c: General linux system control interface
 */

#include <linux/sysctl.h>
#include <linux/printk.h>
#include <linux/capability.h>
#include <linux/ratelimit.h>
#include "internal.h"

static const int ten_thousand = 10000;

static int min_msg_loglevel = LOGLEVEL_EMERG;
static int max_msg_loglevel = LOGLEVEL_DEBUG;

static int proc_dointvec_minmax_sysadmin(const struct ctl_table *table, int write,
				void *buffer, size_t *lenp, loff_t *ppos)
{
	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	return proc_dointvec_minmax(table, write, buffer, lenp, ppos);
}

static int do_proc_dointvec_console_loglevel(bool *negp, unsigned long *lvalp,
					     int *valp,
					     int write, void *data)
{
	int level, ret;

	/*
	 * If writing, first do so via a temporary local int so we can
	 * bounds-check it before touching *valp.
	 */
	int *intp = write ? &level : valp;

	ret = do_proc_dointvec_conv(negp, lvalp, intp, write, data);
	if (ret)
		return ret;

	if (write) {
		if (level != console_clamp_loglevel(level))
			return -ERANGE;

		/*
		 * Honour the administrator-configured minimum console
		 * loglevel (third element of kernel.printk).  This mirrors
		 * the syslog() and sysfs control paths so that once the floor
		 * is raised we do not let this sysctl silently bypass it.
		 */
		if (minimum_console_loglevel > CONSOLE_LOGLEVEL_MIN &&
		    level < minimum_console_loglevel)
			level = minimum_console_loglevel;

		WRITE_ONCE(*valp, level);
	}

	return 0;
}

static int proc_dointvec_console_loglevel(const struct ctl_table *table,
					  int write, void *buffer, size_t *lenp,
					  loff_t *ppos)
{
	return do_proc_dointvec(table, write, buffer, lenp, ppos,
			       do_proc_dointvec_console_loglevel, NULL);
}

static const struct ctl_table printk_sysctls[] = {
	{
		.procname	= "printk",
		.data		= &console_loglevel,
		.maxlen		= 4*sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "printk_ratelimit",
		.data		= &printk_ratelimit_state.interval,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_jiffies,
	},
	{
		.procname	= "printk_ratelimit_burst",
		.data		= &printk_ratelimit_state.burst,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "printk_delay",
		.data		= &printk_delay_msec,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= (void *)&ten_thousand,
	},
	{
		.procname	= "printk_devkmsg",
		.data		= devkmsg_log_str,
		.maxlen		= DEVKMSG_STR_MAX_SIZE,
		.mode		= 0644,
		.proc_handler	= devkmsg_sysctl_set_loglvl,
	},
	{
		.procname	= "dmesg_restrict",
		.data		= &dmesg_restrict,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax_sysadmin,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "kptr_restrict",
		.data		= &kptr_restrict,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax_sysadmin,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO,
	},
	{
		.procname	= "console_loglevel",
		.data		= &console_loglevel,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_console_loglevel,
	},
	{
		.procname	= "default_message_loglevel",
		.data		= &default_message_loglevel,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &min_msg_loglevel,
		.extra2		= &max_msg_loglevel,
	},
};

void __init printk_sysctl_init(void)
{
	register_sysctl_init("kernel", printk_sysctls);
}
