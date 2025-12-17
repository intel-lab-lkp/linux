// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

#include <linux/cpuhotplug.h>
#include <linux/glob.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/suspend.h>
#include <linux/seq_file.h>

#include "internals.h"
#include "irq_moderation.h"

/*
 * Management code for Global Software Interrupt Moderation (GSIM)
 *
 * Some platforms show reduced I/O performance when the total device interrupt
 * rate across the entire platform becomes too high. To address the problem,
 * GSIM uses a hook after running the handler to measure global and per-CPU
 * interrupt rates, compare them with configurable targets, and implements
 * independent, per-CPU software moderation delays.
 *
 * Configuration is controlled at boot time via module parameters
 *
 *     irq_moderation.${NAME}=${VALUE}
 *
 * or at runtime via /proc/irq/soft_moderation
 *
 *     echo "${NAME}=${VALUE}" > /proc/irq/soft_moderation
 *
 * Supported parameters:
 *
 *   delay_us (default 0, suggested 100, 0 off, range 0-500)
 *       Maximum moderation delay. A reasonable range is 20-100. Higher values
 *       can be useful if the hardirq handler has long runtimes.
 *
 *   target_intr_rate (default 0, suggested 1000000, 0 off, range 0-50000000)
 *       The total interrupt rate above which moderation kicks in.
 *       Not particularly critical, a value in the 500K-1M range is usually ok.
 *
 *   hardirq_percent (default 0, suggested 70, 0 off, range 0-100)
 *       The hardirq percentage above which moderation kicks in.
 *       50-90 is a reasonable range.
 *
 *       FIXED MODERATION mode requires target_intr_rate=0, hardirq_percent=0
 *
 *   update_ms (default 5, range 1-100)
 *       How often the load is measured and moderation delay updated.
 *
 *   enable_list (comma-separated list of handlers or interrupt names)
 *       Enable moderation at creation for interrupts whose name or handler
 *       functions match patterns in this list. Example:
 *       "nvme_irq(),eth*,vfio_msihandler()"
 *
 * Moderation can be enabled/disabled dynamically for individual interrupts with
 *
 *     echo 1 > /proc/irq/NN/soft_moderation # use 0 to disable
 *
 * === MONITORING ===
 *
 * cat /proc/irq/soft_moderation shows configuration and per-CPU/global statistics.
 *
 * === ARCHITECTURE ===
 *
 * INTERRUPT HANDLING:
 * - irq_moderation_hook() runs under desc->lock right after the interrupt handler.
 *   If the interrupt must be moderated, it sets IRQD_IRQ_INPROGRESS, calls
 *   __disable_irq() adds the irq_desc to a per-CPU list of moderated interrupts,
 *   and starts a moderation timer if not yet active.
 * - desc->handler is modified so that when called on a moderated irq_desc it
 *   calls mask_irq(), sets IRQS_PENDING and returns immediately.
 * - the timer callback drains the moderation list: on each irq_desc it acquires
 *   desc->lock, and if desc->action != NULL calls __enable_irq(), possibly calling
 *   the handler if IRQS_PENDING is set.
 *
 * INTERRUPT TEARDOWN
 * is protected by IRQD_IRQ_INPROGRESS and checking desc->action != NULL.
 * This works because free_irq() runs in two steps:
 * - first clear desc->action (under lock),
 * - then call synchronize_irq(), which blocks on IRQD_IRQ_INPROGRESS
 *   before freeing resources.
 * When the moderation timer races with free_irq() we can have two cases:
 * 1. timer runs before clearing desc->action. In this case __enable_irq()
 *    is valid and the subsequent free_irq() will complete as intended
 * 2. desc->action is cleared before the timer runs. In this case synchronize_irq()
 *    will block until the timer expires (remember moderation delays are very short,
 *    comparable to C-state exit times), __enable_irq() will not be run,
 *    and free_irq() will complete successfully.
 *
 * INTERRUPT MIGRATION
 * is protected by IRQD_IRQ_INPROGRESS that prevents running the handler on the
 * new CPU while an interrupt is moderated.
 *
 * HOTPLUG
 * During CPU shutdown, the kernel moves timers and reassigns interrupt affinity
 * to a new CPU. The easiest way and most robust way to guarantee that pending
 * events are handled correctly is to use a per-CPU "moderation_on" flag and
 * hotplug callbacks on CPUHP_AP_ONLINE_DYN:
 * - on setup, set the flag. That will allow interrupts to be moderated.
 * - on shutdown, with interrupts disabled, 1. clear the flag thus preventing
 *   more interrupts to be moderated on that CPU, 2. flush the list of moderated
 *   interrupts (as if the timer had fired), and 3. cancel the timer.
 * This avoids depending with the internals of the up/down sequence.
 *
 * SUSPEND
 * Register a PM notifier to handle PM_SUSPEND_PREPARE and PM_POST_RESTORE as
 * hotplug shutdown and setup events. The hotplug callbacks are also invoked
 * during suspend to/resume from disk.
 *
 * BOOT PROCESSOR
 * Hotplug callbacks are not invoked for the boot processor.
 * However the boot processor is the last one to go, and since there is
 * no other place to run the timer callbacks, they will be run where they
 * are supposed to.
 */

/* Recommended values. */
struct irq_mod_info irq_mod_info ____cacheline_aligned = {
	.update_ms		= 5,
	.increase_factor	= MIN_SCALING_FACTOR,
	.scale_cpus		= 200,
};

module_param_named(delay_us, irq_mod_info.delay_us, uint, 0444);
MODULE_PARM_DESC(delay_us, "Max moderation delay us, 0 = moderation off, range 0-500.");

module_param_named(hardirq_percent, irq_mod_info.hardirq_percent, uint, 0444);
MODULE_PARM_DESC(hardirq_percent, "Target max hardirq percentage, 0 off, range 0-100.");

module_param_named(target_intr_rate, irq_mod_info.target_intr_rate, uint, 0444);
MODULE_PARM_DESC(target_intr_rate, "Target max interrupt rate, 0 off, range 0-50000000.");

module_param_named(update_ms, irq_mod_info.update_ms, uint, 0444);
MODULE_PARM_DESC(update_ms, "Update interval in milliseconds, range 1-100");

DEFINE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

DEFINE_STATIC_KEY_FALSE(irq_moderation_enabled_key);

/* Initialize moderation state, used in desc_set_defaults() */
void irq_moderation_init_fields(struct irq_desc_mod *mod)
{
	INIT_LIST_HEAD(&mod->ms_node);
	mod->enable = false;
}

/*
 * irq_moderation.enable_list is a comma-separated list of patterns to match
 * against the name eg "eth*" or handler e.g "nvme_irq()" of the interrupt.
 * There is one active buffer and one used for updates, so replacement just
 * needs to swap the pointer.
 */
#define MAX_CONFIG_LEN 4096
static struct enable_list {
	char buf[MAX_CONFIG_LEN];
	char buf2[MAX_CONFIG_LEN];
	char *cur;
	uint len;
	struct rw_semaphore rwsem;
} patterns = { .cur = patterns.buf, .rwsem = __RWSEM_INITIALIZER(patterns.rwsem) };

static int set_enable_list(const char *val, const struct kernel_param *kp)
{
	uint i, len = strlen(val);
	char *dst;

	if (len >= MAX_CONFIG_LEN - 1)
		return -E2BIG;
	if (val[len - 1] == '\n')
		len--;

	/* Serialized by procfs. */
	dst = patterns.cur == patterns.buf ? patterns.buf2 : patterns.buf;
	/* Copy and split the string on commas. */
	for (i = 0; i < len; i++)
		dst[i] = val[i] == ',' ?  '\0' : val[i];
	dst[i] = '\0';
	guard(rwsem_write)(&patterns.rwsem);
	patterns.cur = dst;
	patterns.len = len;
	return 0;
}

static int get_enable_list(char *buf, const struct kernel_param *kp)
{
	int i;

	/* Join the patterns with commas. */
	guard(rwsem_read)(&patterns.rwsem);
	for (i = 0; i < patterns.len; i++)
		buf[i] = patterns.cur[i] == '\0' ?  ',' : patterns.cur[i];
	buf[i] = '\0';
	return patterns.len;
}

static const struct kernel_param_ops enable_list_ops = {
	.set = set_enable_list,
	.get = get_enable_list,
};

module_param_cb(enable_list, &enable_list_ops, NULL, 0444);

/* Called early in __setup_irq() without desc->lock. */
bool irq_moderation_get_default(const struct irqaction *act)
{
	guard(rwsem_read)(&patterns.rwsem);
	for (int arg = 0; arg < 3; arg++) {
		/* buf includes room for "()". */
		char buf[KSYM_SYMBOL_LEN + 3];
		const char *name;

		if (arg == 0) {
			name = act->name;
			if (!name)
				continue;
		} else {
			irq_handler_t fn = arg == 1 ? act->handler : act->thread_fn;

			if (lookup_symbol_name((ulong)fn, buf))
				continue;
			memcpy(buf + strlen(buf), "()", 3);
			name = buf;
		}
		for (int i = 0; i < patterns.len; i += 1 + strlen(patterns.cur + i))
			if (glob_match(patterns.cur + i, name))
				return true;
	}
	return false;
}

static int set_mode(struct irq_desc *desc, bool enable)
{
	lockdep_assert_held(&desc->lock);
	if (enable) {
		struct irq_data *irqd = &desc->irq_data;
		struct irq_chip *chip = irqd->chip;

		/* Moderation is only supported in specific cases. */
		enable &= !irqd_is_level_type(irqd);
		enable &= irqd_is_single_target(irqd);
		enable &= !chip->irq_bus_lock && !chip->irq_bus_sync_unlock;
		enable &= chip->irq_mask && chip->irq_unmask;
		enable &= desc->handle_irq == handle_edge_irq ||
				desc->handle_irq == handle_fasteoi_irq;
		if (!enable)
			return -EOPNOTSUPP;
	}
	desc->mod.enable = enable;
	return 0;
}

/* Called with desc->lock held in __setup_irq(). */
void irq_moderation_enable(struct irq_desc *desc)
{
	set_mode(desc, true);
}

#pragma clang diagnostic error "-Wformat"
/* Print statistics */
static int moderation_show(struct seq_file *p, void *v)
{
	ulong intr_rate = 0, irq_high = 0, my_irq_high = 0, hardirq_high = 0;
	uint delay_us = irq_mod_info.delay_us;
	u64 now = ktime_get_ns();
	int j, active_cpus = 0;
	char *buf;

#define HEAD_FMT "%5s  %8s  %8s  %4s  %8s  %11s  %11s  %11s  %11s  %11s  %9s\n"
#define BODY_FMT "%5u  %8u  %8u  %4u  %8u  %11u  %11u  %11u  %11u  %11u  %9u\n"

	seq_printf(p, HEAD_FMT,
		   "# CPU", "irq/s", "my_irq/s", "cpus", "delay_ns",
		   "irq_hi", "my_irq_hi", "hardirq_hi", "timer_set",
		   "enqueue", "stray_irq");

	for_each_possible_cpu(j) {
		struct irq_mod_state *ms = per_cpu_ptr(&irq_mod_state, j);
		/* Watch out, epoch start_ns is 64 bits. */
		u64 epoch_start_ns = atomic64_read((atomic64_t *)&ms->epoch_start_ns);
		s64 age_ms = min((now - epoch_start_ns) / NSEC_PER_MSEC, (u64)999999);

		if (age_ms < 10000) {
			/* Average intr_rate over recently active CPUs. */
			active_cpus++;
			intr_rate += ms->intr_rate;
		} else {
			age_ms = -1;
			ms->intr_rate = 0;
			ms->my_intr_rate = 0;
			ms->scaled_cpu_count = 0;
			ms->mod_ns = 0;
		}

		irq_high += ms->irq_high;
		my_irq_high += ms->my_irq_high;
		hardirq_high += ms->hardirq_high;

		seq_printf(p, BODY_FMT,
			   j, ms->intr_rate, ms->my_intr_rate,
			   (ms->scaled_cpu_count + 128) / 256,
			   ms->mod_ns, ms->irq_high, ms->my_irq_high,
			   ms->hardirq_high, ms->timer_set, ms->enqueue,
			   ms->stray_irq);
	}

	buf = kmalloc(MAX_CONFIG_LEN, GFP_KERNEL);
	if (buf)
		get_enable_list(buf, NULL);
	seq_printf(p, "\n"
		   "enabled              %s\n"
		   "enable_list          '%s'\n"
		   "delay_us             %u\n"
		   "target_intr_rate     %u\n"
		   "hardirq_percent      %u\n"
		   "update_ms            %u\n"
		   "scale_cpus           %u\n",
		   str_yes_no(delay_us > 0),
		   buf ? : "",
		   delay_us,
		   irq_mod_info.target_intr_rate, irq_mod_info.hardirq_percent,
		   irq_mod_info.update_ms, irq_mod_info.scale_cpus);
	if (buf)
		kfree(buf);

	seq_printf(p,
		   "intr_rate            %lu\n"
		   "irq_high             %lu\n"
		   "my_irq_high          %lu\n"
		   "hardirq_percent_high %lu\n"
		   "total_interrupts     %u\n"
		   "total_cpus           %u\n",
		   active_cpus ? intr_rate / active_cpus : 0,
		   irq_high, my_irq_high, hardirq_high,
		   READ_ONCE(*((u32 *)&irq_mod_info.total_intrs)),
		   READ_ONCE(*((u32 *)&irq_mod_info.total_cpus)));

	return 0;
}

static int moderation_open(struct inode *inode, struct file *file)
{
	return single_open(file, moderation_show, pde_data(inode));
}

/* Helpers to set and clamp values from procfs or at init. */
struct param_names {
	const char *name;
	uint *val;
	uint min;
	uint max;
};

static struct param_names param_names[] = {
	{ "delay_us", &irq_mod_info.delay_us, 0, 500 },
	{ "target_intr_rate", &irq_mod_info.target_intr_rate, 0, 50000000 },
	{ "hardirq_percent", &irq_mod_info.hardirq_percent, 0, 100 },
	{ "update_ms", &irq_mod_info.update_ms, 1, 100 },
	/* Entries with no name cannot be set at runtime. */
	{ "", &irq_mod_info.increase_factor, MIN_SCALING_FACTOR, 128 },
	{ "", &irq_mod_info.scale_cpus, 50, 1000 },
	/* val = NULL indicates a special entry for enable_list. */
	{ "enable_list", NULL },
};

static void update_enable_key(void)
{
	struct static_key_false *key = &irq_moderation_enabled_key;

	if (irq_mod_info.delay_us != 0)
		static_branch_enable(key);
	else
		static_branch_disable(key);
}

static ssize_t set_enable_list_from_proc(const char __user *buf, int count, int ofs)
{
	char *cmd;

	if (count >= MAX_CONFIG_LEN)
		return -EINVAL;
	cmd = kmalloc(count + 1, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;
	if (copy_from_user(cmd, buf, count)) {
		kfree(cmd);
		return -EFAULT;
	}
	set_enable_list(cmd + ofs, NULL);
	kfree(cmd);
	return count;
}

static ssize_t moderation_write(struct file *f, const char __user *buf, size_t count, loff_t *ppos)
{
	uint i, val, copy_len, name_len;
	struct param_names *n;
	char cmd[40];

	copy_len = min(sizeof(cmd) - 1, count);
	if (count == 0)
		return -EINVAL;
	if (copy_from_user(cmd, buf, copy_len))
		return -EFAULT;
	cmd[copy_len] = '\0';
	for (i = 0, n = param_names;  i < ARRAY_SIZE(param_names); i++, n++) {
		name_len = strlen(n->name);
		if (name_len < 1 || copy_len < name_len + 2 || strncmp(cmd, n->name, name_len) ||
		    cmd[name_len] != '=')
			continue;
		if (n->val == NULL)
			return set_enable_list_from_proc(buf, count, name_len + 1);
		if (kstrtouint(cmd + name_len + 1, 0, &val))
			return -EINVAL;
		WRITE_ONCE(*(n->val), clamp(val, n->min, n->max));
		if (n->val == &irq_mod_info.delay_us)
			update_enable_key();
		/* Notify parameter update. */
		irq_mod_info.version++;
		return count;
	}
	return -EINVAL;
}

static const struct proc_ops proc_ops = {
	.proc_open	= moderation_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= moderation_write,
};

/* Handlers for /proc/irq/NN/soft_moderation */
static int mode_show(struct seq_file *p, void *v)
{
	struct irq_desc *desc = p->private;

	if (!desc)
		return -ENOENT;

	seq_puts(p, desc->mod.enable ? "on\n" : "off\n");
	return 0;
}

static ssize_t mode_write(struct file *f, const char __user *buf, size_t count, loff_t *ppos)
{
	struct irq_desc *desc = (struct irq_desc *)pde_data(file_inode(f));
	char cmd[40];
	bool enable;
	int ret;

	if (!desc)
		return -ENOENT;
	if (count == 0 || count + 1 > sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, buf, count))
		return -EFAULT;
	cmd[count] = '\0';

	ret = kstrtobool(cmd, &enable);
	if (!ret) {
		guard(raw_spinlock_irqsave)(&desc->lock);
		ret = set_mode(desc, enable);
	}
	return ret ? : count;
}

static int mode_open(struct inode *inode, struct file *file)
{
	return single_open(file, mode_show, pde_data(inode));
}

static const struct proc_ops mode_ops = {
	.proc_open	= mode_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= mode_write,
};

void irq_moderation_procfs_add(struct irq_desc *desc, umode_t umode)
{
	proc_create_data("soft_moderation", umode, desc->dir, &mode_ops, desc);
}

void irq_moderation_procfs_remove(struct irq_desc *desc)
{
	remove_proc_entry("soft_moderation", desc->dir);
}

/* Used on timer expiration or CPU shutdown. */
static void drain_desc_list(struct irq_mod_state *ms)
{
	struct irq_desc *desc, *next;

	/* Remove from list and enable interrupts back. */
	list_for_each_entry_safe(desc, next, &ms->descs, mod.ms_node) {
		guard(raw_spinlock)(&desc->lock);
		list_del_init(&desc->mod.ms_node);
		irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);
		/* Protect against competing free_irq(). */
		if (desc->action)
			__enable_irq(desc);
	}
}

static enum hrtimer_restart timer_callback(struct hrtimer *timer)
{
	struct irq_mod_state *ms = this_cpu_ptr(&irq_mod_state);

	drain_desc_list(ms);
	/* Prepare to accumulate next moderation delay. */
	ms->sleep_ns = 0;
	return HRTIMER_NORESTART;
}

/* Hotplug callback for setup. */
static int cpu_setup_cb(uint cpu)
{
	struct irq_mod_state *ms = this_cpu_ptr(&irq_mod_state);

	if (ms->moderation_on)
		return 0;
	hrtimer_setup(&ms->timer, timer_callback,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	INIT_LIST_HEAD(&ms->descs);
	smp_mb();
	ms->moderation_on = true;
	return 0;
}

/*
 * Hotplug callback for shutdown.
 * Mark the CPU as offline for moderation, and drain the list of masked
 * interrupts. Any subsequent interrupt on this CPU will not be
 * moderated, but they will be on the new target.
 */
static int cpu_remove_cb(uint cpu)
{
	struct irq_mod_state *ms = this_cpu_ptr(&irq_mod_state);
	unsigned long flags;

	local_irq_save(flags);
	ms->moderation_on = false;
	drain_desc_list(ms);
	hrtimer_cancel(&ms->timer);
	local_irq_restore(flags);
	return 0;
}

static void(mod_pm_prepare_cb)(void *arg)
{
	cpu_remove_cb(smp_processor_id());
}

static void(mod_pm_resume_cb)(void *arg)
{
	cpu_setup_cb(smp_processor_id());
}

static int mod_pm_notifier_cb( struct notifier_block *nb, unsigned long event, void *unused)
{
	switch (event) {
	case PM_SUSPEND_PREPARE:
		on_each_cpu(mod_pm_prepare_cb, NULL, 1);
		break;
	case PM_POST_SUSPEND:
		on_each_cpu(mod_pm_resume_cb, NULL, 1);
		break;
	}
	return NOTIFY_OK;
}

struct notifier_block mod_nb = {
	.notifier_call = mod_pm_notifier_cb,
	.priority = 100,
};

static void clamp_parameter(uint *dst, uint val)
{
	struct param_names *n = param_names;

	for (int i = 0; i < ARRAY_SIZE(param_names); i++, n++) {
		if (dst == n->val) {
			WRITE_ONCE(*dst, clamp(val, n->min, n->max));
			return;
		}
	}
}

static int __init init_irq_moderation(void)
{
	/* Clamp all initial values to the allowed range, update version. */
	for (uint *cur = &irq_mod_info.delay_us; cur < irq_mod_info.pad; cur++)
		clamp_parameter(cur, *cur);
	irq_mod_info.version++;

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "irq_moderation", cpu_setup_cb, cpu_remove_cb);
	register_pm_notifier(&mod_nb);

	update_enable_key();

	proc_create_data("irq/soft_moderation", 0644, NULL, &proc_ops, NULL);
	return 0;
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Luigi Rizzo <lrizzo@google.com>");
MODULE_DESCRIPTION("Global Software Interrupt Moderation");
module_init(init_irq_moderation);
