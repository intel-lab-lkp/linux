// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

#include <linux/cpuhotplug.h>
#include <linux/glob.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kallsyms.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/suspend.h>
#include <linux/seq_file.h>

#include "internals.h"
#include "irq_moderation.h"

/*
 * Global Software Interrupt Moderation (GSIM)
 *
 * Some platforms show reduced I/O performance when the total device interrupt
 * rate across the entire platform becomes too high. To address the problem,
 * GSIM uses a hook after running the handler to implement software interrupt
 * moderation with programmable delay.
 *
 * Configuration is done at runtime via procfs
 *   echo ${VALUE} > /proc/irq/soft_moderation/${NAME}
 *
 * Supported parameters:
 *
 *   delay_us (default 0, suggested 100, 0 off, range 0-500)
 *       Maximum moderation delay. A reasonable range is 20-100. Higher values
 *       can be useful if the hardirq handler has long runtimes.
 *
 * Moderation can be enabled/disabled dynamically for individual interrupts with
 *   echo 1 > /proc/irq/NN/soft_moderation # use 0 to disable
 *
 * Monitoring of per-cpu and global statistics is available via procfs
 *   cat /proc/irq/soft_moderation/stats
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
 * events are handled correctly is to use a per-CPU "moderation_allowed" flag
 * and hotplug callbacks on CPUHP_AP_ONLINE_DYN:
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
};

DEFINE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

DEFINE_STATIC_KEY_FALSE(irq_moderation_enabled_key);

static void update_enable_key(void)
{
	if (irq_mod_info.delay_us != 0)
		static_branch_enable(&irq_moderation_enabled_key);
	else
		static_branch_disable(&irq_moderation_enabled_key);
}

/* Actually start moderation. */
bool irq_moderation_do_start(struct irq_desc *desc, struct irq_mod_state *m)
{
	lockdep_assert_irqs_disabled();

	if (!hrtimer_is_queued(&m->timer)) {
		const u32 min_delay_ns = 10000;
		const u64 slack_ns = 2000;

		/* Accumulate sleep time, no moderation if too small. */
		m->sleep_ns += m->mod_ns;
		if (m->sleep_ns < min_delay_ns)
			return false;
		/* We need moderation, start the timer. */
		m->timer_set++;
		hrtimer_start_range_ns(&m->timer, ns_to_ktime(m->sleep_ns),
				       slack_ns, HRTIMER_MODE_REL_PINNED_HARD);
	}

	/*
	 * Add to the timer list and __disable_irq() to prevent serving subsequent
	 * interrupts.
	 */
	if (!list_empty(&desc->mod_state.ms_node)) {
		/* Very unlikely, stray interrupt while moderated. */
		m->stray_irq++;
	} else {
		m->enqueue++;
		list_add(&desc->mod_state.ms_node, &m->descs);
		__disable_irq(desc);
	}
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS | IRQD_IRQ_MODERATED);
	return true;
}

/* Initialize moderation state, used in desc_set_defaults() */
void irq_moderation_init_fields(struct irq_desc_mod *mod_state)
{
	INIT_LIST_HEAD(&mod_state->ms_node);
}

static int set_mode(struct irq_desc *desc, bool enable)
{
	struct irq_data *irqd = &desc->irq_data;
	struct irq_chip *chip = irqd->chip;

	lockdep_assert_held(&desc->lock);

	if (!enable) {
		irq_settings_clr_and_set(desc, _IRQ_SW_MODERATION, 0);
		return 0;
	}

	/* Moderation is only supported in specific cases. */
	enable &= !irqd_is_level_type(irqd);
	enable &= irqd_is_single_target(irqd);
	enable &= !chip->irq_bus_lock && !chip->irq_bus_sync_unlock;
	enable &= chip->irq_mask && chip->irq_unmask;
	enable &= desc->handle_irq == handle_edge_irq || desc->handle_irq == handle_fasteoi_irq;
	if (!enable)
		return -EOPNOTSUPP;

	irq_settings_clr_and_set(desc, 0, _IRQ_SW_MODERATION);
	return 0;
}

/* Helpers to set and clamp values from procfs or at init. */
struct swmod_param {
	const char	*name;
	int		(*wr)(struct swmod_param *n, const char __user *s, size_t count);
	void		(*rd)(struct seq_file *p);
	void		*val;
	u32		min;
	u32		max;
};

static int swmod_wr_u32(struct swmod_param *n, const char __user *s, size_t count)
{
	u32 res;
	int ret = kstrtouint_from_user(s, count, 0, &res);

	if (!ret) {
		WRITE_ONCE(*(u32 *)(n->val), clamp(res, n->min, n->max));
		ret = count;
	}
	return ret;
}

static void swmod_rd_u32(struct seq_file *p)
{
	struct swmod_param *n = p->private;

	seq_printf(p, "%u\n", *(u32 *)(n->val));
}

static int swmod_wr_delay(struct swmod_param *n, const char __user *s, size_t count)
{
	int ret = swmod_wr_u32(n, s, count);

	if (ret >= 0)
		update_enable_key();
	return ret;
}

#define HEAD_FMT "%5s  %8s  %11s  %11s  %9s\n"
#define BODY_FMT "%5u  %8u  %11u  %11u  %9u\n"

#pragma clang diagnostic error "-Wformat"

/* Print statistics */
static void rd_stats(struct seq_file *p)
{
	uint delay_us = irq_mod_info.delay_us;
	int cpu;

	seq_printf(p, HEAD_FMT,
		   "# CPU", "delay_ns", "timer_set", "enqueue", "stray_irq");

	for_each_possible_cpu(cpu) {
		struct irq_mod_state cur;

		/* Copy statistics, will only use some 32bit values, races ok. */
		data_race(cur = *per_cpu_ptr(&irq_mod_state, cpu));
		seq_printf(p, BODY_FMT,
			   cpu, cur.mod_ns, cur.timer_set, cur.enqueue, cur.stray_irq);
	}

	seq_printf(p, "\n"
		   "enabled              %s\n"
		   "delay_us             %u\n",
		   str_yes_no(delay_us > 0),
		   delay_us);
}

static int moderation_show(struct seq_file *p, void *v)
{
	struct swmod_param *n = p->private;

	if (!n || !n->rd)
		return -EINVAL;
	n->rd(p);
	return 0;
}

static int moderation_open(struct inode *inode, struct file *file)
{
	return single_open(file, moderation_show, pde_data(inode));
}

static struct swmod_param param_names[] = {
	{ "delay_us", swmod_wr_delay, swmod_rd_u32, &irq_mod_info.delay_us, 0, 500 },
	{ "stats", NULL, rd_stats},
};

static ssize_t moderation_write(struct file *f, const char __user *buf, size_t count, loff_t *ppos)
{
	struct swmod_param *n = (struct swmod_param *)pde_data(file_inode(f));

	return n && n->wr ? n->wr(n, buf, count) : -EINVAL;
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

	seq_puts(p, irq_settings_moderation_allowed(desc) ? "on\n" : "off\n");
	return 0;
}

static ssize_t mode_write(struct file *f, const char __user *buf, size_t count, loff_t *ppos)
{
	struct irq_desc *desc = (struct irq_desc *)pde_data(file_inode(f));
	bool enable;
	int ret = kstrtobool_from_user(buf, count, &enable);

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
static void drain_desc_list(struct irq_mod_state *m)
{
	struct irq_desc *desc, *next;

	/* Remove from list and enable interrupts back. */
	list_for_each_entry_safe(desc, next, &m->descs, mod_state.ms_node) {
		guard(raw_spinlock)(&desc->lock);
		list_del_init(&desc->mod_state.ms_node);
		irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS | IRQD_IRQ_MODERATED);
		/* Protect against competing free_irq(). */
		if (desc->action)
			__enable_irq(desc);
	}
}

static enum hrtimer_restart timer_callback(struct hrtimer *timer)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	lockdep_assert_irqs_disabled();

	drain_desc_list(m);
	/* Prepare to accumulate next moderation delay. */
	m->sleep_ns = 0;
	return HRTIMER_NORESTART;
}

/* Hotplug callback for setup. */
static int cpu_setup_cb(uint cpu)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	hrtimer_setup(&m->timer, timer_callback,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	INIT_LIST_HEAD(&m->descs);
	m->moderation_allowed = true;
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
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	guard(irqsave)();
	m->moderation_allowed = false;
	drain_desc_list(m);
	hrtimer_cancel(&m->timer);
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

static int mod_pm_notifier_cb(struct notifier_block *nb, unsigned long event, void *unused)
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
	.notifier_call	= mod_pm_notifier_cb,
	.priority	= 100,
};

static void __init clamp_parameter(u32 *dst, u32 val)
{
	struct swmod_param *n = param_names;

	for (int i = 0; i < ARRAY_SIZE(param_names); i++, n++) {
		if (dst == n->val) {
			WRITE_ONCE(*dst, clamp(val, n->min, n->max));
			return;
		}
	}
}

static int __init init_irq_moderation(void)
{
	struct proc_dir_entry *dir;
	struct swmod_param *n;
	int i;

	/* Clamp all initial values to the allowed range. */
	for (uint *cur = &irq_mod_info.delay_us; cur < irq_mod_info.params_end; cur++)
		clamp_parameter(cur, *cur);

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "irq_moderation", cpu_setup_cb, cpu_remove_cb);
	register_pm_notifier(&mod_nb);

	update_enable_key();

	dir = proc_mkdir("irq/soft_moderation", NULL);
	if (!dir)
		return 0;
	for (i = 0, n = param_names; i < ARRAY_SIZE(param_names); i++, n++)
		proc_create_data(n->name, n->wr ? 0644 : 0444, dir, &proc_ops, n);
	return 0;
}

device_initcall(init_irq_moderation);
