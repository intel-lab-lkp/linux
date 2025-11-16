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

#ifdef CONFIG_X86
#include <asm/apic.h>
#include <asm/irq_remapping.h>
#else
static inline bool posted_msi_supported(void) { return false; }
#endif

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
 * - Intel only: using "intremap=posted_msi", all the above is done in
 *   sysvec_posted_msi_notification(). In this case all host device interrupts
 *   are subject to moderation.
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
 *     FIXED MODERATION mode requires target_irq_rate=0, hardirq_percent=0
 *
 *   target_irq_rate (default 0, suggested 1000000, 0 off, range 0..50M)
 *     The total irq rate above which moderation kicks in.
 *     Not particularly critical, a value in the 500K-1M range is usually ok.
 *
 *   hardirq_percent (default 0, suggested 70, 0 off, range 10..100)
 *     The hardirq percentage above which moderation kicks in.
 *     50-90 is a reasonable range.
 *
 *   update_ms (default 5, range 1...100)
 *     How often the load is measured and moderation delay updated.
 *
 * Moderation can be enabled/disabled for individual interrupts with
 *
 *    echo "on" > /proc/irq/NN/soft_moderation # use "off" to disable
 *
 * For selected drivers, the default can also be supplied via module parameters
 *
 *	${DRIVER}.soft_moderation=1
 *
 * === MONITORING ===
 *
 * cat /proc/irq/soft_moderation shows per-CPU and global statistics.
 *
 */

/* Recommended values for the control loop. */
struct irq_mod_info irq_mod_info ____cacheline_aligned = {
	.update_ms		= 5,
	.scale_cpus		= 200,
	.decay_factor		= 16,
	.grow_factor		= 8,
};

/* Boot time value, copled to irq_mod_info.delay_us after init. */
static uint mod_delay_us;
module_param_named(delay_us, mod_delay_us, uint, 0444);
MODULE_PARM_DESC(delay_us, "Max moderation delay us, 0 = moderation off, range 0-500.");

module_param_named(timer_rounds, irq_mod_info.timer_rounds, uint, 0444);
MODULE_PARM_DESC(timer_rounds, "How many timer polls once moderation triggers, range 0-20.");

module_param_named(hardirq_percent, irq_mod_info.hardirq_percent, uint, 0444);
MODULE_PARM_DESC(hardirq_percent, "Target max hardirq percentage, 0 off, range 0-100.");

module_param_named(target_irq_rate, irq_mod_info.target_irq_rate, uint, 0444);
MODULE_PARM_DESC(target_irq_rate, "Target max interrupt rate, 0 off, range 0-50000000.");

module_param_named(update_ms, irq_mod_info.update_ms, uint, 0444);
MODULE_PARM_DESC(update_ms, "Update interval in milliseconds, range 1-100");

DEFINE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

DEFINE_STATIC_KEY_FALSE(irq_moderation_enabled_key);
EXPORT_SYMBOL(irq_moderation_enabled_key);

static inline void smooth_avg(u32 *dst, u32 val, u32 steps)
{
	*dst = ((64 - steps) * *dst + steps * val) / 64;
}

/* Measure and assess time spent in hardirq. */
static inline bool hardirq_high(struct irq_mod_state *ms, u64 delta_time, u32 hardirq_percent)
{
	bool above_threshold = false;

	if (IS_ENABLED(CONFIG_IRQ_TIME_ACCOUNTING)) {
		u64 irqtime, cur = kcpustat_this_cpu->cpustat[CPUTIME_IRQ];

		irqtime = cur - ms->last_irqtime;
		ms->last_irqtime = cur;

		above_threshold = irqtime * 100 > delta_time * hardirq_percent;
		ms->hardirq_high += above_threshold;
	}
	return above_threshold;
}

/* Measure and assess total and per-CPU interrupt rates. */
static inline bool irqrate_high(struct irq_mod_state *ms, u64 delta_time,
				u32 target_rate, u32 steps)
{
	u64 irq_rate, my_irq_rate, tmp, delta_irqs, active_cpus;
	bool my_rate_high, global_rate_high;

	my_irq_rate = (ms->irq_count * NSEC_PER_SEC) / delta_time;
	/* Accumulate global counter and compute global irq rate. */
	tmp = atomic_long_add_return(ms->irq_count, &irq_mod_info.total_intrs);
	ms->irq_count = 1;
	delta_irqs = tmp - ms->last_total_irqs;
	ms->last_total_irqs = tmp;
	irq_rate = (delta_irqs * NSEC_PER_SEC) / delta_time;

	/*
	 * Count how many CPUs handled interrupts in the last interval, needed
	 * to determine the per-CPU target (target_rate / active_cpus).
	 * Each active CPU increments the global counter approximately every
	 * update_ns. Scale the value by (update_ns / delta_time) to get the
	 * correct value. Also apply rounding and make sure active_cpus > 0.
	 */
	tmp = atomic_long_add_return(1, &irq_mod_info.total_cpus);
	active_cpus = tmp - ms->last_total_cpus;
	ms->last_total_cpus = tmp;
	active_cpus = (active_cpus * ms->update_ns + delta_time / 2) / delta_time;
	if (active_cpus < 1)
		active_cpus = 1;

	/* Compare with global and per-CPU targets. */
	global_rate_high = irq_rate > target_rate;
	my_rate_high = my_irq_rate * active_cpus * irq_mod_info.scale_cpus > target_rate * 100;

	/* Statistics. */
	smooth_avg(&ms->irq_rate, irq_rate, steps);
	smooth_avg(&ms->my_irq_rate, my_irq_rate, steps);
	smooth_avg(&ms->scaled_cpu_count, active_cpus * 256, steps);
	ms->my_irq_high += my_rate_high;
	ms->irq_high += global_rate_high;

	/* Moderate on this CPU only if both global and local rates are high. */
	return global_rate_high && my_rate_high;
}

/* Adjust the moderation delay, called at most every update_ns. */
void __irq_moderation_adjust_delay(struct irq_mod_state *ms, u64 delta_time)
{
	u32 hardirq_percent = READ_ONCE(irq_mod_info.hardirq_percent);
	u32 target_rate = READ_ONCE(irq_mod_info.target_irq_rate);
	bool below_target = true;
	u32 steps;

	if (target_rate == 0 && hardirq_percent == 0) {
		/* Use fixed moderation delay. */
		ms->mod_ns = ms->delay_ns;
		ms->irq_rate = 0;
		ms->my_irq_rate = 0;
		ms->scaled_cpu_count = 0;
		return;
	}

	/* Compute decay steps based on elapsed time, bound to a reasonable value. */
	steps = delta_time > 10 * ms->update_ns ? 10 : 1 + (delta_time / ms->update_ns);

	if (target_rate > 0 && irqrate_high(ms, delta_time, target_rate, steps))
		below_target = false;

	if (hardirq_percent > 0 && hardirq_high(ms, delta_time, hardirq_percent))
		below_target = false;

	/* Controller: adjust delay with exponential decay/grow. */
	if (below_target) {
		ms->mod_ns -= ms->mod_ns * steps / (steps + irq_mod_info.decay_factor);
		if (ms->mod_ns < 100)
			ms->mod_ns = 0;
	} else {
		/* Exponential grow does not restart if value is too small. */
		if (ms->mod_ns < 500)
			ms->mod_ns = 500;
		ms->mod_ns += ms->mod_ns * steps / (steps + irq_mod_info.grow_factor);
		if (ms->mod_ns > ms->delay_ns)
			ms->mod_ns = ms->delay_ns;
	}
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

#ifdef CONFIG_X86_POSTED_MSI
	if (ms->kick_posted_msi) {
		if (ms->rounds_left == 0)
			ms->kick_posted_msi = false;
		/* Next call will be from timer, count it conditionally. */
		ms->dont_count = !irq_mod_info.count_timer_calls;
		ms->timer_calls++;
		apic->send_IPI_self(POSTED_MSI_NOTIFICATION_VECTOR);
	}
#endif

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

/* irq_to_desc() is not exported. Wrap it for use in drivers. */
void irq_moderation_set_mode(int irq, bool enable)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc)
		set_moderation_mode(desc, enable);
}
EXPORT_SYMBOL(irq_moderation_set_mode);

#pragma clang diagnostic error "-Wformat"
/* Print statistics */
static int moderation_show(struct seq_file *p, void *v)
{
	ulong irq_rate = 0, irq_high = 0, my_irq_high = 0, hardirq_high = 0;
	uint delay_us = irq_mod_info.delay_us;
	u64 now = ktime_get_ns();
	int j, active_cpus = 0;

#define HEAD_FMT "%5s  %8s  %8s  %4s  %4s  %8s  %11s  %11s  %11s  %11s  %11s  %11s  %11s  %9s\n"
#define BODY_FMT "%5u  %8u  %8u  %4u  %4u  %8u  %11u  %11u  %11u  %11u  %11u  %11u  %11u  %9u\n"

	seq_printf(p, HEAD_FMT,
		   "# CPU", "irq/s", "my_irq/s", "cpus", "srcs", "delay_ns",
		   "irq_hi", "my_irq_hi", "hardirq_hi", "timer_set",
		   "disable_irq", "from_msi", "timer_calls", "stray_irq");

	for_each_possible_cpu(j) {
		struct irq_mod_state *ms = per_cpu_ptr(&irq_mod_state, j);
		u64 age_ms = min((now - ms->last_ns) / NSEC_PER_MSEC, (u64)999999);

		if (age_ms < 10000) {
			/* Average irq_rate over recently active CPUs. */
			active_cpus++;
			irq_rate += ms->irq_rate;
		} else {
			ms->irq_rate = 0;
			ms->my_irq_rate = 0;
			ms->scaled_cpu_count = 64;
			ms->scaled_src_count = 64;
			ms->mod_ns = 0;
		}

		irq_high += ms->irq_high;
		my_irq_high += ms->my_irq_high;
		hardirq_high += ms->hardirq_high;

		seq_printf(p, BODY_FMT,
			   j, ms->irq_rate, ms->my_irq_rate,
			   (ms->scaled_cpu_count + 128) / 256,
			   (ms->scaled_src_count + 128) / 256,
			   ms->mod_ns, ms->irq_high, ms->my_irq_high,
			   ms->hardirq_high, ms->timer_set, ms->disable_irq,
			   ms->from_posted_msi, ms->timer_calls, ms->stray_irq);
	}

	seq_printf(p, "\n"
		   "enabled              %s%s\n"
		   "delay_us             %u\n"
		   "timer_rounds         %u\n"
		   "target_irq_rate      %u\n"
		   "hardirq_percent      %u\n"
		   "update_ms            %u\n"
		   "scale_cpus           %u\n"
		   "count_timer_calls    %s\n"
		   "count_msi_calls      %s\n"
		   "decay_factor         %u\n"
		   "grow_factor          %u\n",
		   str_yes_no(delay_us > 0),
		   posted_msi_supported() ? " (also on posted_msi)" : "",
		   delay_us, irq_mod_info.timer_rounds,
		   irq_mod_info.target_irq_rate, irq_mod_info.hardirq_percent,
		   irq_mod_info.update_ms, irq_mod_info.scale_cpus,
		   str_yes_no(irq_mod_info.count_timer_calls),
		   str_yes_no(irq_mod_info.count_msi_calls),
		   irq_mod_info.decay_factor, irq_mod_info.grow_factor);

	seq_printf(p,
		   "irq_rate             %lu\n"
		   "irq_high             %lu\n"
		   "my_irq_high          %lu\n"
		   "hardirq_percent_high %lu\n"
		   "total_interrupts     %lu\n"
		   "total_cpus           %lu\n",
		   active_cpus ? irq_rate / active_cpus : 0,
		   irq_high, my_irq_high, hardirq_high,
		   READ_ONCE(*((ulong *)&irq_mod_info.total_intrs)),
		   READ_ONCE(*((ulong *)&irq_mod_info.total_cpus)));

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
	{ "target_irq_rate", &irq_mod_info.target_irq_rate, 0, 50000000 },
	{ "hardirq_percent", &irq_mod_info.hardirq_percent, 0, 100 },
	{ "update_ms", &irq_mod_info.update_ms, 1, 100 },
	/* Empty entry indicates the following are not settable from procfs. */
	{},
	{ "scale_cpus", &irq_mod_info.scale_cpus, 50, 1000 },
	{ "count_timer_calls", &irq_mod_info.count_timer_calls, 0, 1 },
	{ "count_msi_calls", &irq_mod_info.count_msi_calls, 0, 1 },
	{ "decay_factor", &irq_mod_info.decay_factor, 8, 64 },
	{ "grow_factor", &irq_mod_info.grow_factor, 8, 64 },
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
	if (ret) {
		/* extra helpers for prodkernel */
		if (cmd[count - 1] == '\n')
			cmd[count - 1] = '\0';
		ret = 0;
		if (!strcmp(cmd, "managed"))
			irqd_set(&desc->irq_data, IRQD_AFFINITY_MANAGED);
		else if (!strcmp(cmd, "unmanaged"))
			irqd_clear(&desc->irq_data, IRQD_AFFINITY_MANAGED);
		else
			ret = -EINVAL;
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
