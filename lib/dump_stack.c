// SPDX-License-Identifier: GPL-2.0
/*
 * Provide a default dump_stack() function for architectures
 * which don't implement their own.
 */

#include <linux/kernel.h>
#include <linux/buildid.h>
#include <linux/cache.h>
#include <linux/export.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/kexec.h>
#include <linux/utsname.h>
#include <linux/stop_machine.h>

static char dump_stack_arch_desc_str[128] __ro_after_init;
static const char *dump_stack_arch_desc_ptr = dump_stack_arch_desc_str;

/**
 * dump_stack_set_arch_desc - set arch-specific str to show with task dumps
 * @fmt: printf-style format string
 * @...: arguments for the format string
 *
 * The configured string will be printed right after utsname during task
 * dumps.  Usually used to add arch-specific system identifiers.  If an
 * arch wants to make use of such an ID string, it should initialize this
 * as soon as possible during boot.
 */
void dump_stack_set_arch_desc(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf(dump_stack_arch_desc_str, sizeof(dump_stack_arch_desc_str),
		  fmt, args);
	va_end(args);
}

/**
 * dump_stack_update_arch_desc() - Update the arch description string at runtime.
 * @fmt: printf-style format string
 * @...: arguments for the format string
 *
 * A runtime counterpart of dump_stack_set_arch_desc(). Arch code
 * should use this when the arch description set at boot potentially
 * has become inaccurate, such as after a guest migration.
 *
 * Context: May sleep.
 */
void dump_stack_update_arch_desc(const char *fmt, ...)
{
	static DEFINE_SPINLOCK(arch_desc_update_lock);
	const char *old;
	const char *new;
	va_list args;

	va_start(args, fmt);
	new = kvasprintf(GFP_KERNEL, fmt, args);
	va_end(args);

	if (!new)
		return;

	spin_lock(&arch_desc_update_lock);
	old = rcu_replace_pointer(dump_stack_arch_desc_ptr, new,
				  lockdep_is_held(&arch_desc_update_lock));
	spin_unlock(&arch_desc_update_lock);

	/*
	 * Avoid freeing the static buffer initialized during boot.
	 */
	if (old == dump_stack_arch_desc_str)
		return;

	kfree_rcu_mightsleep(old);
}

#if IS_ENABLED(CONFIG_STACKTRACE_BUILD_ID)
#define BUILD_ID_FMT " %20phN"
#define BUILD_ID_VAL vmlinux_build_id
#else
#define BUILD_ID_FMT "%s"
#define BUILD_ID_VAL ""
#endif

/**
 * dump_stack_print_info - print generic debug info for dump_stack()
 * @log_lvl: log level
 *
 * Arch-specific dump_stack() implementations can use this function to
 * print out the same debug information as the generic dump_stack().
 */
void dump_stack_print_info(const char *log_lvl)
{
	const char *arch_str;

	printk("%sCPU: %d PID: %d Comm: %.20s %s%s %s %.*s" BUILD_ID_FMT "\n",
	       log_lvl, raw_smp_processor_id(), current->pid, current->comm,
	       kexec_crash_loaded() ? "Kdump: loaded " : "",
	       print_tainted(),
	       init_utsname()->release,
	       (int)strcspn(init_utsname()->version, " "),
	       init_utsname()->version, BUILD_ID_VAL);

	rcu_read_lock();
	arch_str = rcu_dereference(dump_stack_arch_desc_ptr);
	if (arch_str[0] != '\0')
		printk("%sHardware name: %s\n", log_lvl, arch_str);
	rcu_read_unlock();

	print_worker_info(log_lvl, current);
	print_stop_info(log_lvl, current);
}

/**
 * show_regs_print_info - print generic debug info for show_regs()
 * @log_lvl: log level
 *
 * show_regs() implementations can use this function to print out generic
 * debug information.
 */
void show_regs_print_info(const char *log_lvl)
{
	dump_stack_print_info(log_lvl);
}

static void __dump_stack(const char *log_lvl)
{
	dump_stack_print_info(log_lvl);
	show_stack(NULL, NULL, log_lvl);
}

/**
 * dump_stack_lvl - dump the current task information and its stack trace
 * @log_lvl: log level
 *
 * Architectures can override this implementation by implementing its own.
 */
asmlinkage __visible void dump_stack_lvl(const char *log_lvl)
{
	unsigned long flags;

	/*
	 * Permit this cpu to perform nested stack dumps while serialising
	 * against other CPUs
	 */
	printk_cpu_sync_get_irqsave(flags);
	__dump_stack(log_lvl);
	printk_cpu_sync_put_irqrestore(flags);
}
EXPORT_SYMBOL(dump_stack_lvl);

asmlinkage __visible void dump_stack(void)
{
	dump_stack_lvl(KERN_DEFAULT);
}
EXPORT_SYMBOL(dump_stack);
