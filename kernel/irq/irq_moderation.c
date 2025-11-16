// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

#include <linux/cpuhotplug.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>

#include "internals.h"
#include "irq_moderation.h"

/*
 * Platform-wide software interrupt moderation.
 *
 * see Documentation/core-api/irq/irq-moderation.rst
 *
 * === MOTIVATION AND OPERATION ===
 *
 * Some platforms show reduced I/O performance when the total device interrupt
 * rate across the entire platform becomes too high. This code implements
 * per-CPU adaptive moderation based on the total interrupt rate, as opposed
 * to conventional moderation that operates separately on each source.
 *
 * It computes the total interrupt rate and number of sources, and uses the
 * information to adaptively disable individual interrupts for small amounts
 * of time using per-CPU hrtimers. Specifically:
 *
 * - a hook in handle_irq_event(), which applies only on sources configured
 *   to use moderation, updates statistics and check whether we need
 *   moderation on that CPU/irq. If so, calls disable_irq_nosync() and starts
 *   an hrtimer with appropriate delay.
 *
 * - the timer callback calls enable_irq() for all disabled interrupts on that
 *   CPU. That in turn will generate interrupts if there are pending events.
 *
 * === CONFIGURATION ===
 *
 * The following can be controlled at boot time via module parameters
 *
 *     irq_moderation.${NAME}=${VALUE}
 *
 * or at runtime by writing
 *
 *     echo "${NAME}=${VALUE}" > /proc/irq/soft_moderation
 *
 *   delay_us (default 0, suggested 100, range 0-500, 0 DISABLES MODERATION)
 *     Fixed or maximum moderation delay.  A reasonable range is 20..100, higher
 *     values can be useful if the hardirq handler is performing a significant
 *     amount of work.
 *
 *   timer_rounds (default 0, max 20)
 *     Once moderation triggers, periodically run handler zero or more
 *     times using a timer rather than interrupts. This is similar to
 *     napi_defer_hard_irqs on NICs.
 *     A small value may help control load in interrupt-challenged platforms.
 *
 *
 *   update_ms (default 5, range 1...100)
 *     How often moderation delay is updated.
 *
 * Moderation can be enabled/disabled for individual interrupts with
 *
 *    echo "on" > /proc/irq/NN/soft_moderation # use "off" to disable
 *
 * === MONITORING ===
 *
 * cat /proc/irq/soft_moderation shows per-CPU and global statistics.
 *
 */

/* Recommended values for the control loop. */
struct irq_mod_info irq_mod_info ____cacheline_aligned = {
	.update_ms		= 5,
};

/* Boot time value, copled to irq_mod_info.delay_us after init. */
static uint mod_delay_us;
module_param_named(delay_us, mod_delay_us, uint, 0444);
MODULE_PARM_DESC(delay_us, "Max moderation delay us, 0 = moderation off, range 0-500.");

module_param_named(timer_rounds, irq_mod_info.timer_rounds, uint, 0444);
MODULE_PARM_DESC(timer_rounds, "How many timer polls once moderation triggers, range 0-20.");

module_param_named(update_ms, irq_mod_info.update_ms, uint, 0444);
MODULE_PARM_DESC(update_ms, "Update interval in milliseconds, range 1-100");

DEFINE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

DEFINE_STATIC_KEY_FALSE(irq_moderation_enabled_key);
EXPORT_SYMBOL(irq_moderation_enabled_key);

static inline void smooth_avg(u32 *dst, u32 val, u32 steps)
{
	*dst = ((64 - steps) * *dst + steps * val) / 64;
}

/* Moderation timer handler. */
static enum hrtimer_restart timer_cb(struct hrtimer *timer)
{
	struct irq_mod_state *ms = this_cpu_ptr(&irq_mod_state);
	struct irq_desc *desc, *next;
	uint srcs = 0;

	ms->timer_fire++;
	WARN_ONCE(ms->timer_set != ms->timer_fire,
		  "CPU %d timer set %d fire %d (lost events?)\n",
		  smp_processor_id(), ms->timer_set, ms->timer_fire);

	ms->rounds_left--;

	if (ms->rounds_left > 0) {
		/* Timer still alive, just call the handlers. */
		list_for_each_entry_safe(desc, next, &ms->descs, mod.ms_node) {
			ms->irq_count += irq_mod_info.count_timer_calls;
			ms->timer_calls++;
			handle_irq_event_percpu(desc);
		}
		ms->timer_set++;
		hrtimer_forward_now(&ms->timer, ms->sleep_ns);
		return HRTIMER_RESTART;
	}

	/* Last round, remove from list and enable_irq(). */
	list_for_each_entry_safe(desc, next, &ms->descs, mod.ms_node) {
		list_del(&desc->mod.ms_node);
		INIT_LIST_HEAD(&desc->mod.ms_node);
		srcs++;
		ms->enable_irq++;
		enable_irq(desc->irq_data.irq);
	}
	smooth_avg(&ms->scaled_src_count, srcs * 256, 1);

	/* Prepare to accumulate next moderation delay. */
	ms->sleep_ns = 0;

	WARN_ONCE(ms->disable_irq != ms->enable_irq,
		  "CPU %d irq disable %d enable %d (%s)\n",
		  smp_processor_id(), ms->disable_irq, ms->enable_irq,
		  "bookkeeping error, some irq will be stuck");

	return HRTIMER_NORESTART;
}

/* Initialize moderation state, used in desc_set_defaults() */
void irq_moderation_init_fields(struct irq_desc_mod *mod)
{
	INIT_LIST_HEAD(&mod->ms_node);
	mod->enable = false;
}

static inline int set_moderation_mode(struct irq_desc *desc, bool enable)
{
	struct irq_data *irqd = &desc->irq_data;
	struct irq_chip *chip = desc->irq_data.chip;

	/* Moderation is supported only in specific cases. */
	if (enable) {
		if (irqd_is_level_type(irqd) || !irqd_is_single_target(irqd) ||
		    chip->irq_bus_lock || chip->irq_bus_sync_unlock)
			return -EOPNOTSUPP;
	}
	desc->mod.enable = enable;
	return 0;
}

#pragma clang diagnostic error "-Wformat"
/* Print statistics */
static int moderation_show(struct seq_file *p, void *v)
{
	uint delay_us = irq_mod_info.delay_us;
	int j;

#define HEAD_FMT "%5s  %8s  %8s  %4s  %4s  %8s  %11s  %11s  %11s  %11s  %11s  %11s  %11s  %9s\n"
#define BODY_FMT "%5u  %8u  %8u  %4u  %4u  %8u  %11u  %11u  %11u  %11u  %11u  %11u  %11u  %9u\n"

	seq_printf(p, HEAD_FMT,
		   "# CPU", "irq/s", "my_irq/s", "cpus", "srcs", "delay_ns",
		   "irq_hi", "my_irq_hi", "hardirq_hi", "timer_set",
		   "disable_irq", "from_msi", "timer_calls", "stray_irq");

	for_each_possible_cpu(j) {
		struct irq_mod_state *ms = per_cpu_ptr(&irq_mod_state, j);

		seq_printf(p, BODY_FMT,
			   j, ms->irq_rate, ms->my_irq_rate,
			   (ms->scaled_cpu_count + 128) / 256,
			   (ms->scaled_src_count + 128) / 256,
			   ms->mod_ns, ms->irq_high, ms->my_irq_high,
			   ms->hardirq_high, ms->timer_set, ms->disable_irq,
			   ms->from_posted_msi, ms->timer_calls, ms->stray_irq);
	}

	seq_printf(p, "\n"
		   "enabled              %s\n"
		   "delay_us             %u\n"
		   "timer_rounds         %u\n",
		   str_yes_no(delay_us > 0),
		   delay_us, irq_mod_info.timer_rounds);

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
	{ "timer_rounds", &irq_mod_info.timer_rounds, 0, 20 },
	{ "update_ms", &irq_mod_info.update_ms, 1, 100 },
	/* Empty entry indicates the following are not settable from procfs. */
	{},
	{ "count_timer_calls", &irq_mod_info.count_timer_calls, 0, 1 },
};

static void update_enable_key(void)
{
	bool newval = irq_mod_info.delay_us != 0;

	if (newval != static_key_enabled(&irq_moderation_enabled_key)) {
		if (newval)
			static_branch_enable(&irq_moderation_enabled_key);
		else
			static_branch_disable(&irq_moderation_enabled_key);
	}
}

static ssize_t moderation_write(struct file *f, const char __user *buf, size_t count, loff_t *ppos)
{
	struct param_names *n = param_names;
	char cmd[40];
	uint i, l, val;

	if (count == 0 || count + 1 > sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, buf, count))
		return -EFAULT;
	cmd[count] = '\0';
	for (i = 0;  i < ARRAY_SIZE(param_names) && n->name; i++, n++) {
		l = strlen(n->name);
		if (count < l + 2 || strncmp(cmd, n->name, l) || cmd[l] != '=')
			continue;
		if (kstrtouint(cmd + l + 1, 0, &val))
			return -EINVAL;
		WRITE_ONCE(*(n->val), clamp(val, n->min, n->max));
		if (n->val == &irq_mod_info.delay_us)
			update_enable_key();
		/* Record last parameter change, for use in the control loop. */
		irq_mod_info.procfs_write_ns = ktime_get_ns();
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

	seq_printf(p, "%s irq %u trigger 0x%x %s %smanaged %slazy handle_irq %pB\n",
		   desc->mod.enable ? "on" : "off", desc->irq_data.irq,
		   irqd_get_trigger_type(&desc->irq_data),
		   irqd_is_level_type(&desc->irq_data) ? "Level" : "Edge",
		   irqd_affinity_is_managed(&desc->irq_data) ? "" : "un",
		   irq_settings_disable_unlazy(desc) ? "un" : "", desc->handle_irq
		   );
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
	if (!ret)
		ret = set_moderation_mode(desc, enable);
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

/* Per-CPU state initialization */
static void irq_moderation_percpu_init(void *data)
{
	struct irq_mod_state *ms = this_cpu_ptr(&irq_mod_state);

	hrtimer_setup(&ms->timer, timer_cb, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	INIT_LIST_HEAD(&ms->descs);
}

static int cpuhp_setup_cb(uint cpu)
{
	irq_moderation_percpu_init(NULL);
	return 0;
}

static void clamp_parameter(uint *dst, uint val)
{
	struct param_names *n = param_names;
	uint i;

	for (i = 0; i < ARRAY_SIZE(param_names); i++, n++) {
		if (dst == n->val) {
			*dst = clamp(val, n->min, n->max);
			return;
		}
	}
}

static int __init init_irq_moderation(void)
{
	uint *cur;

	on_each_cpu(irq_moderation_percpu_init, NULL, 1);
	cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "moderation:online", cpuhp_setup_cb, NULL);

	/* Clamp all initial values to the allowed range. */
	for (cur = &irq_mod_info.target_irq_rate; cur < irq_mod_info.pad; cur++)
		clamp_parameter(cur, *cur);

	/* Finally, set delay_us to enable moderation if needed. */
	clamp_parameter(&irq_mod_info.delay_us, mod_delay_us);
	update_enable_key();

	proc_create_data("irq/soft_moderation", 0644, NULL, &proc_ops, NULL);
	return 0;
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Luigi Rizzo <lrizzo@google.com>");
MODULE_DESCRIPTION("Platform wide software interrupt moderation");
module_init(init_irq_moderation);
