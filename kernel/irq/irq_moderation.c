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
 * GSIM uses a hook after running the handler to measure global and per-CPU
 * interrupt rates, compare them with configurable targets, and implements
 * independent, per-CPU software moderation delays.
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
 *   scale_cpus (default 150, range 50-1000)
 *       Small update_ms may lead to underestimate the number of CPUs
 *       simultaneously handling interrupts, and the opposite can happen
 *       with very large values. This parameter may help correct the value,
 *       though it is not recommended to modify the default unless there are
 *       very strong reasons.
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
	.increase_factor	= MIN_SCALING_FACTOR,
	.scale_cpus		= 150,
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

/* Functions called in handle_*_irq(). */

/*
 * Compute smoothed average between old and cur. 'steps' is used
 * to approximate applying the smoothing multiple times.
 */
static inline u32 smooth_avg(u32 old, u32 cur, u32 steps)
{
	const u32 smooth_factor = 64;

	steps = min(steps, smooth_factor - 1);
	return ((smooth_factor - steps) * old + steps * cur) / smooth_factor;
}

/* Measure and assess time spent in hardirq. */
static inline bool hardirq_high(struct irq_mod_state *m, u32 hardirq_percent)
{
	bool above_threshold;
	u64 irqtime, cur;

	if (!IS_ENABLED(CONFIG_IRQ_TIME_ACCOUNTING))
		return false;

	cur = kcpustat_this_cpu->cpustat[CPUTIME_IRQ];
	irqtime = cur - m->last_irqtime;
	m->last_irqtime = cur;

	above_threshold = irqtime * 100 > (u64)m->epoch_ns * hardirq_percent;
	m->hardirq_high += above_threshold;
	return above_threshold;
}

/* Measure and assess total and per-CPU interrupt rates. */
static inline bool irqrate_high(struct irq_mod_state *m, u32 target_rate, u32 steps)
{
	u32 global_intr_rate, local_intr_rate, delta_intrs, active_cpus, tmp;
	bool local_rate_high, global_rate_high;

	local_intr_rate = ((u64)m->intr_count * NSEC_PER_SEC) / m->epoch_ns;

	/* Accumulate global counter and compute global interrupt rate. */
	tmp = atomic_add_return(m->intr_count, &irq_mod_info.total_intrs);
	m->intr_count = 1;
	delta_intrs = tmp - m->last_total_intrs;
	m->last_total_intrs = tmp;
	global_intr_rate = ((u64)delta_intrs * NSEC_PER_SEC) / m->epoch_ns;

	/*
	 * Count how many CPUs handled interrupts in the last epoch, needed
	 * to determine the per-CPU target (target_rate / active_cpus).
	 * Each active CPU increments the global counter approximately every
	 * update_ns. Scale the value by (update_ns / m->epoch_ns) to get the
	 * correct value. Also apply rounding and make sure active_cpus > 0.
	 */
	tmp = atomic_add_return(1, &irq_mod_info.total_cpus);
	active_cpus = tmp - m->last_total_cpus;
	m->last_total_cpus = tmp;
	active_cpus = (active_cpus * m->update_ns + m->epoch_ns / 2) / m->epoch_ns;
	if (active_cpus < 1)
		active_cpus = 1;

	/* Compare with global and per-CPU targets. */
	global_rate_high = global_intr_rate > target_rate;

	/*
	 * Short epochs may lead to underestimate the number of active CPUs.
	 * Apply a scaling factor to compensate. This may make the controller
	 * a bit more aggressive but does not harm system throughput.
	 */
	local_rate_high = local_intr_rate * active_cpus * irq_mod_info.scale_cpus > target_rate * 100;

	/* Statistics. */
	m->global_intr_rate = smooth_avg(m->global_intr_rate, global_intr_rate, steps);
	m->local_intr_rate = smooth_avg(m->local_intr_rate, local_intr_rate, steps);
	m->scaled_cpu_count = smooth_avg(m->scaled_cpu_count, active_cpus * 256, steps);
	m->local_irq_high += local_rate_high;
	m->global_irq_high += global_rate_high;

	/* Moderate on this CPU only if both global and local rates are high. */
	return global_rate_high && local_rate_high;
}

/* Periodic adjustment, called once per epoch. */
void irq_moderation_update_epoch(struct irq_mod_state *m)
{
	const u32 hardirq_percent = READ_ONCE(irq_mod_info.hardirq_percent);
	const u32 target_rate = READ_ONCE(irq_mod_info.target_intr_rate);
	const u32 min_delay_ns = 500;
	bool above_target = false;
	u32 steps;

	/*
	 * If any of the configuration parameter changes, read the main ones
	 * (delay_ns, update_ns), and set the adaptive delay, mod_ns, to the
	 * maximum value to help converge.
	 * Without that, the system might be already below target_intr_rate
	 * because of saturation on the bus (the very problem GSIM is trying
	 * to address) and that would block the control loop.
	 * Setting mod_ns to the highest value (if chosen properly) can reduce
	 * the interrupt rate below target_intr_rate and let the controller
	 * gradually reach the target.
	 */
	if (raw_read_seqcount(&irq_mod_info.seq.seqcount) != m->seq) {
		do {
			m->seq = read_seqbegin(&irq_mod_info.seq);
			m->update_ns = READ_ONCE(irq_mod_info.update_ms) * NSEC_PER_MSEC;
			m->mod_ns = READ_ONCE(irq_mod_info.delay_us) * NSEC_PER_USEC;
			m->delay_ns = m->mod_ns;
		} while (read_seqretry(&irq_mod_info.seq, m->seq));
	}

	if (target_rate == 0 && hardirq_percent == 0) {
		/* Use fixed moderation delay. */
		m->mod_ns = m->delay_ns;
		m->global_intr_rate = 0;
		m->local_intr_rate = 0;
		m->scaled_cpu_count = 0;
		return;
	}

	/*
	 * To scale values X by a factor (1 +/- 1/F) every "update_ns" we do
	 *	X := X * (1 +/- 1/F)
	 * If the interval is N times longer, applying the formula N times gives
	 *	X := X * ((1 +/- 1/F) ** N)
	 * We don't want to deal floating point or exponentials, and we cap N
	 * to some small value < F . This leads to an approximated formula
	 *	X := X * (1 +/- N/F)
	 * The variable steps below is the number N of steps.
	 */
	steps = clamp(m->epoch_ns / m->update_ns, 1u, MIN_SCALING_FACTOR - 1u);

	if (target_rate > 0 && irqrate_high(m, target_rate, steps))
		above_target = true;

	if (hardirq_percent > 0 && hardirq_high(m, hardirq_percent))
		above_target = true;

	/*
	 * Controller: adjust delay with exponential increase or decrease.
	 *
	 * Note the different constants: we increase fast (smaller factor)
	 * to aggressively slow down when the interrupt rate goes up,
	 * but decrease slowly (larger factor) because reducing the delay can
	 * drive up the interrupt rate and we don't want to create load spikes.
	 */
	if (above_target) {
		const u32 increase_factor = READ_ONCE(irq_mod_info.increase_factor);

		/* Make sure the value is large enough for the exponential to grow. */
		if (m->mod_ns < min_delay_ns)
			m->mod_ns = min_delay_ns;
		m->mod_ns += m->mod_ns * steps / increase_factor;
		if (m->mod_ns > m->delay_ns)
			m->mod_ns = m->delay_ns;
	} else {
		const u32 decrease_factor = 2 * READ_ONCE(irq_mod_info.increase_factor);

		m->mod_ns -= m->mod_ns * steps / decrease_factor;
		/* Round down to 0 values that are too small to bother. */
		if (m->mod_ns < min_delay_ns)
			m->mod_ns = 0;
	}
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

/* Control functions. */

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
		write_seqlock(&irq_mod_info.seq);
		WRITE_ONCE(*(u32 *)(n->val), clamp(res, n->min, n->max));
		write_sequnlock(&irq_mod_info.seq);
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

#define HEAD_FMT "%5s  %8s  %10s  %4s  %8s  %11s  %11s  %11s  %11s  %11s  %9s\n"
#define BODY_FMT "%5u  %8u  %10u  %4u  %8u  %11u  %11u  %11u  %11u  %11u  %9u\n"

#pragma clang diagnostic error "-Wformat"

/* Print statistics */
static void rd_stats(struct seq_file *p)
{
	ulong global_intr_rate = 0, global_irq_high = 0;
	ulong local_irq_high = 0, hardirq_high = 0;
	uint delay_us = irq_mod_info.delay_us;
	u64 now = ktime_get_ns();
	int cpu, active_cpus = 0;

	seq_printf(p, HEAD_FMT,
		   "# CPU", "irq/s", "loc_irq/s", "cpus", "delay_ns",
		   "irq_hi", "loc_irq_hi", "hardirq_hi", "timer_set",
		   "enqueue", "stray_irq");

	for_each_possible_cpu(cpu) {
		struct irq_mod_state cur, *m = per_cpu_ptr(&irq_mod_state, cpu);
		u64 epoch_start_ns;
		bool recent;

		/* Accumulate and print only recent samples */
		epoch_start_ns = atomic64_read(&m->epoch_start_ns);
		recent = (now - epoch_start_ns) < 10 * NSEC_PER_SEC;

		/* Copy statistics, will only use some 32bit values, races ok. */
		data_race(cur = *per_cpu_ptr(&irq_mod_state, cpu));
		if (recent) {
			active_cpus++;
			global_intr_rate += cur.global_intr_rate;
		}

		global_irq_high += cur.global_irq_high;
		local_irq_high += cur.local_irq_high;
		hardirq_high += cur.hardirq_high;

		seq_printf(p, BODY_FMT,
			   cpu,
			   recent * cur.global_intr_rate,
			   recent * cur.local_intr_rate,
			   recent * (cur.scaled_cpu_count + 128) / 256,
			   recent * cur.mod_ns,
			   cur.global_irq_high,
			   cur.local_irq_high,
			   cur.hardirq_high,
			   cur.timer_set,
			   cur.enqueue,
			   cur.stray_irq);
	}

	seq_printf(p, "\n"
		   "enabled              %s\n"
		   "delay_us             %u\n"
		   "target_intr_rate     %u\n"
		   "hardirq_percent      %u\n"
		   "update_ms            %u\n"
		   "scale_cpus           %u\n",
		   str_yes_no(delay_us > 0),
		   delay_us,
		   irq_mod_info.target_intr_rate, irq_mod_info.hardirq_percent,
		   irq_mod_info.update_ms, irq_mod_info.scale_cpus);

	seq_printf(p,
		   "intr_rate            %lu\n"
		   "irq_high             %lu\n"
		   "my_irq_high          %lu\n"
		   "hardirq_percent_high %lu\n"
		   "total_interrupts     %u\n"
		   "total_cpus           %u\n",
		   active_cpus ? global_intr_rate / active_cpus : 0,
		   global_irq_high, local_irq_high, hardirq_high,
		   READ_ONCE(*((u32 *)&irq_mod_info.total_intrs)),
		   READ_ONCE(*((u32 *)&irq_mod_info.total_cpus)));
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
	{ "target_intr_rate", swmod_wr_u32, swmod_rd_u32, &irq_mod_info.target_intr_rate, 0, 50000000 },
	{ "hardirq_percent", swmod_wr_u32, swmod_rd_u32, &irq_mod_info.hardirq_percent, 0, 100 },
	{ "update_ms", swmod_wr_u32, swmod_rd_u32, &irq_mod_info.update_ms, 1, 100 },
	{ "increase_factor", swmod_wr_u32, NULL, &irq_mod_info.increase_factor, MIN_SCALING_FACTOR, 128 },
	{ "scale_cpus", swmod_wr_u32, swmod_rd_u32, &irq_mod_info.scale_cpus, 50, 1000 },
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
	seqlock_init(&irq_mod_info.seq);

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "irq_moderation", cpu_setup_cb, cpu_remove_cb);
	register_pm_notifier(&mod_nb);

	update_enable_key();

	dir = proc_mkdir("irq/soft_moderation", NULL);
	if (!dir)
		return 0;
	for (i = 0, n = param_names; i < ARRAY_SIZE(param_names); i++, n++) {
		if (!n->rd)
			continue;
		proc_create_data(n->name, n->wr ? 0644 : 0444, dir, &proc_ops, n);
	}
	return 0;
}

device_initcall(init_irq_moderation);
