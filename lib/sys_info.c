// SPDX-License-Identifier: GPL-2.0-only
#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/console.h>
#include <linux/log2.h>
#include <linux/kernel.h>
#include <linux/ftrace.h>
#include <linux/nmi.h>
#include <linux/sched/debug.h>
#include <linux/string.h>
#include <linux/sysctl.h>

#include <linux/sys_info.h>

static const char * const si_names[] = {
	[ilog2(SYS_INFO_TASKS)]			= "tasks",
	[ilog2(SYS_INFO_MEM)]			= "mem",
	[ilog2(SYS_INFO_TIMERS)]		= "timers",
	[ilog2(SYS_INFO_LOCKS)]			= "locks",
	[ilog2(SYS_INFO_FTRACE)]		= "ftrace",
	[ilog2(SYS_INFO_PANIC_CONSOLE_REPLAY)]	= "",
	[ilog2(SYS_INFO_ALL_BT)]		= "all_bt",
	[ilog2(SYS_INFO_BLOCKED_TASKS)]		= "blocked_tasks",
};

/*
 * Default kernel sys_info mask.
 * If a kernel module calls sys_info() with "parameter == 0", then
 * this mask will be used.
 */
static unsigned long kernel_si_mask;

/* Expecting string like "xxx_sys_info=tasks,mem,timers,locks,ftrace,..." */
unsigned long sys_info_parse_param(char *str)
{
	unsigned long si_bits = 0;
	char *s, *name;
	int i;

	s = str;
	while ((name = strsep(&s, ",")) && *name) {
		i = match_string(si_names, ARRAY_SIZE(si_names), name);
		if (i >= 0)
			__set_bit(i, &si_bits);
	}

	return si_bits;
}

#ifdef CONFIG_SYSCTL

static int sys_info_write_handler(const struct ctl_table *table,
				  void *buffer, size_t *lenp, loff_t *ppos,
				  unsigned long *si_bits_global)
{
	unsigned long si_bits;
	int ret;

	ret = proc_dostring(table, 1, buffer, lenp, ppos);
	if (ret)
		return ret;

	si_bits = sys_info_parse_param(table->data);

	/* The access to the global value is not synchronized. */
	WRITE_ONCE(*si_bits_global, si_bits);

	return 0;
}

static int sys_info_read_handler(const struct ctl_table *table,
				 void *buffer, size_t *lenp, loff_t *ppos,
				 unsigned long *si_bits_global)
{
	unsigned long si_bits;
	unsigned int len = 0;
	char *delim = "";
	unsigned int i;

	/* The access to the global value is not synchronized. */
	si_bits = READ_ONCE(*si_bits_global);

	for_each_set_bit(i, &si_bits, ARRAY_SIZE(si_names)) {
		if (*si_names[i]) {
			len += scnprintf(table->data + len, table->maxlen - len,
					 "%s%s", delim, si_names[i]);
			delim = ",";
		}
	}

	return proc_dostring(table, 0, buffer, lenp, ppos);
}

int sysctl_sys_info_handler(const struct ctl_table *ro_table, int write,
					  void *buffer, size_t *lenp,
					  loff_t *ppos)
{
	struct ctl_table table;
	unsigned int i;
	size_t maxlen;

	maxlen = 0;
	for (i = 0; i < ARRAY_SIZE(si_names); i++)
		maxlen += strlen(si_names[i]) + 1;

	char *names __free(kfree) = kzalloc(maxlen, GFP_KERNEL);
	if (!names)
		return -ENOMEM;

	table = *ro_table;
	table.data = names;
	table.maxlen = maxlen;

	if (write)
		return sys_info_write_handler(&table, buffer, lenp, ppos, ro_table->data);
	else
		return sys_info_read_handler(&table, buffer, lenp, ppos, ro_table->data);
}

static const struct ctl_table sys_info_sysctls[] = {
	{
		.procname	= "kernel_sys_info",
		.data		= &kernel_si_mask,
		.maxlen         = sizeof(kernel_si_mask),
		.mode		= 0644,
		.proc_handler	= sysctl_sys_info_handler,
	},
};

static int __init sys_info_sysctl_init(void)
{
	register_sysctl_init("kernel", sys_info_sysctls);
	return 0;
}
subsys_initcall(sys_info_sysctl_init);
#endif

static void __sys_info(unsigned long si_mask)
{
	if (si_mask & SYS_INFO_TASKS)
		show_state();

	if (si_mask & SYS_INFO_MEM)
		show_mem();

	if (si_mask & SYS_INFO_TIMERS)
		sysrq_timer_list_show();

	if (si_mask & SYS_INFO_LOCKS)
		debug_show_all_locks();

	if (si_mask & SYS_INFO_FTRACE)
		ftrace_dump(DUMP_ALL);

	if (si_mask & SYS_INFO_ALL_BT)
		trigger_all_cpu_backtrace();

	if (si_mask & SYS_INFO_BLOCKED_TASKS)
		show_state_filter(TASK_UNINTERRUPTIBLE);
}

void sys_info(unsigned long si_mask)
{
	__sys_info(si_mask ? : kernel_si_mask);
}

#ifdef CONFIG_SW_WATCHPOINT

/* default 100 ms interval */
static unsigned long watch_interval_ms = 100;
module_param(watch_interval_ms, ulong, 0644);
MODULE_PARM_DESC(watch_interval_ms, "SW watchpoint check interval in ms");

static unsigned long paddr_dram_to_watch;
module_param(paddr_dram_to_watch, ulong, 0644);
MODULE_PARM_DESC(paddr_dram_to_watch, "Physical DRAM address to watch");

static unsigned long *vaddr_dram;

static unsigned long target_dram_val;
module_param(target_dram_val, ulong, 0644);
MODULE_PARM_DESC(target_dram_val, "Target DRAM value to trigger watchpoint");

/* The MMIO address should be 32b aligned */
static unsigned long paddr_mmio_to_watch;
module_param(paddr_mmio_to_watch, ulong, 0644);
MODULE_PARM_DESC(paddr_mmio_to_watch, "Physical MMIO address to watch (32bit aligned)");

static unsigned int *vaddr_mmio;

static unsigned int target_mmio_val;
module_param(target_mmio_val, uint, 0644);
MODULE_PARM_DESC(target_mmio_val, "Target MMIO value to trigger watchpoint");

static bool panic_on_hit;
module_param(panic_on_hit, bool, 0644);
MODULE_PARM_DESC(panic_on_hit, "Panic when watchpoint hits");

static bool hang_on_hit;
module_param(hang_on_hit, bool, 0644);
MODULE_PARM_DESC(hang_on_hit, "Hang when watchpoint hits");

/* Stop the watchpoint timer after first hit */
static bool check_once = true;
module_param(check_once, bool, 0644);
MODULE_PARM_DESC(check_once, "Stop watching after first hit");

static struct timer_list sw_watchpoint_timer;

static void sw_watchpoint_timer_fn(struct timer_list *unused)
{
	bool hit = false;

	if (vaddr_mmio && (*vaddr_mmio == target_mmio_val)) {
		pr_info("MMIO [@0x%lx] hit the target value [0x%x]!\n",
			paddr_mmio_to_watch, target_mmio_val);
		hit = true;
	}

	if (vaddr_dram && (*vaddr_dram == target_dram_val)) {
		pr_info("DRAM [@0x%lx] hit the target value [0x%lx]!\n",
			paddr_dram_to_watch, target_dram_val);
		hit = true;
	}

	if (hit) {
		sys_info(0);

		/* Useful for attaching HW debugger */
		if (hang_on_hit) {
			pr_warn("Will dead loop on this CPU\n");
			while (1);
		}

		/* Could be used to trigger kexec/kdump */
		if (panic_on_hit)
			panic("SW watchpoint hit!");

		if (check_once)
			return;
	}

	mod_timer(&sw_watchpoint_timer, jiffies + msecs_to_jiffies(watch_interval_ms));
}

static int __init sw_watchpoint_timer_init(void)
{
	if (paddr_mmio_to_watch) {
		vaddr_mmio = ioremap(paddr_mmio_to_watch & PAGE_MASK, PAGE_SIZE);
		if (!vaddr_mmio)
			return -ENOMEM;

		vaddr_mmio += (paddr_mmio_to_watch % PAGE_SIZE) / 4;
	}

	if (paddr_dram_to_watch) {
		vaddr_dram = phys_to_virt(paddr_dram_to_watch);
		if (!vaddr_dram)
			return -ENOMEM;
	}

	timer_setup(&sw_watchpoint_timer, sw_watchpoint_timer_fn, 0);
	sw_watchpoint_timer.expires = jiffies + msecs_to_jiffies(watch_interval_ms);
	add_timer(&sw_watchpoint_timer);

	return 0;
}
core_initcall(sw_watchpoint_timer_init);
#endif
