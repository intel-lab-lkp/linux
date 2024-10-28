// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysctl.c: General linux system control interface
 */

#include <linux/sysctl.h>
#include <linux/printk.h>
#include <linux/capability.h>
#include <linux/ratelimit.h>
#include <linux/console.h>
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

static int printk_console_loglevel(const struct ctl_table *table, int write,
				   void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table ltable = *table;
	int ret, level;

	if (!write)
		return proc_dointvec(table, write, buffer, lenp, ppos);

	ltable.data = &level;

	ret = proc_dointvec(&ltable, write, buffer, lenp, ppos);
	if (ret)
		return ret;

	if (level != -1 && level != clamp_loglevel(level))
		return -ERANGE;

	console_loglevel = level;

	return 0;
}

static int printk_sysctl_deprecated(const struct ctl_table *table, int write,
				    void *buffer, size_t *lenp, loff_t *ppos)
{
	int res = proc_dointvec(table, write, buffer, lenp, ppos);

	if (write)
		pr_warn_once("printk: The kernel.printk sysctl is deprecated. Consider using kernel.console_loglevel or kernel.default_message_loglevel instead.\n");

	return res;
}

static struct ctl_table printk_sysctls[] = {
	{
		.procname	= "printk",
		.data		= &console_loglevel,
		.maxlen		= 4*sizeof(int),
		.mode		= 0644,
		.proc_handler	= printk_sysctl_deprecated,
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
		.proc_handler	= printk_console_loglevel,
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
