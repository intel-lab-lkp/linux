// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

/*
 * Copyright (C) 2025-2026 Google LLC
 *
 * Global Software Interrupt Moderation (GSIM) core logic.
 */

#include <linux/cpuhotplug.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kallsyms.h>
#include <linux/kernel_stat.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/suspend.h>

#include "internals.h"
#include "irq_moderation.h"

/*
 * Global Software Interrupt Moderation (GSIM)
 *
 * Some platforms show reduced I/O performance when the total device interrupt
 * rate across the entire platform becomes too high. To address the problem,
 * GSIM runs after the handler to measure global and per-CPU interrupt rates,
 * compares them with configurable targets, and implements independent, per-CPU
 * software moderation delays.
 *
 * Configuration is done at runtime via procfs
 *   echo ${VALUE} > /proc/irq/sw_moderation/${NAME}
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
 * Moderation is allowed/disallowed dynamically for individual interrupts with
 *   echo 1 > /proc/irq/NN/allow_sw_moderation # use 0 to disallow
 *
 * Monitoring of per-cpu and global statistics is available via procfs
 *   cat /proc/irq/sw_moderation/stats
 *
 * === ARCHITECTURE ===
 *
 * INTERRUPT HANDLING (for interrupt types that support moderation)
 * - irq_start_moderation() runs under desc->lock right after the interrupt handler.
 *   If the interrupt must be moderated, sets IRQD_IRQ_INPROGRESS and IRQD_MODERATED,
 *   calls __disable_irq(), adds the irq_desc to a per-CPU list of moderated interrupts,
 *   and starts a moderation timer if not yet active;
 * - handle_xx_irq() is modified so that when called on a moderated irq_desc it
 *   calls mask_irq(), sets IRQS_PENDING and returns immediately;
 * - the timer callback drains the moderation list: on each irq_desc it acquires
 *   desc->lock, and if desc->action != NULL calls __enable_irq(), possibly calling
 *   the handler if IRQS_PENDING is set.
 *
 * INTERRUPT TEARDOWN
 * It is protected by IRQD_IRQ_INPROGRESS and checking desc->action != NULL.
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
 * It is protected by IRQD_IRQ_INPROGRESS that prevents running the handler on the
 * new CPU while an interrupt is moderated.
 *
 * HOTPLUG
 * During CPU shutdown, the kernel moves timers and reassigns interrupt affinity
 * to a new CPU. The easiest way and most robust way to guarantee that pending
 * events are handled correctly is to use a per-CPU "moderation_allowed" flag
 * and hotplug callbacks on CPUHP_AP_ONLINE_DYN (some others are equally good):
 * - on setup, set the flag. That will allow interrupts to be moderated.
 * - on shutdown, with interrupts disabled, 1. clear the flag thus preventing
 *   more interrupts to be moderated on that CPU, 2. flush the list of moderated
 *   interrupts (as if the timer had fired), and 3. cancel the timer.
 * This avoids depending with the internals of the up/down sequence.
 *
 * STATIC ENABLING
 * GSIM is disabled by default (delay_ns = 0). To statically enable it:
 * 1. Initialize irq_mod_params.delay_ns to a non-zero value (e.g., 100000 for 100us)
 *    in kernel/irq/irq_moderation.c.
 * 2. Call irq_settings_set_moderatable(desc) during interrupt allocation
 *    (e.g., in __setup_irq() in kernel/irq/manage.c) for the desired interrupts.
 *
 * SUSPEND & HIBERNATION
 * During Suspend-to-RAM or Suspend-to-Disk (Hibernation), secondary CPUs are
 * taken offline, which triggers the CPU hotplug teardown and setup callbacks.
 * However, the boot processor is never taken offline via hotplug.
 *
 * To ensure the boot processor's GSIM state is safely drained/disabled before
 * suspend, and safely re-enabled after resume/restore, we register a PM notifier.
 *
 * The PM notifier handles all suspend, hibernation, and restore transitions:
 * - On prepare (*_PREPARE): runs mod_pm_prepare_cb() on all CPUs (via IPI)
 *   to clear the allowed flag and drain pending moderated interrupts, and
 *   then cancels GSIM timers on all online CPUs outside IPI context.
 * - On resume/restore (POST_*): runs mod_pm_resume_cb() on all CPUs to
 *   safely re-allow moderation. We do NOT run cpu_setup_cb() here to avoid
 *   dangerous double-initialization of active hrtimers or list heads on
 *   already-online secondary CPUs.
 */

/*
 * GSIM parameters. Initialize delay_ns here to statically enable moderation
 * (e.g. .delay_ns = 100000).
 */

/*
 * Recommended values for the adaptive control loop.
 *
 * update_ns is documented earlier and can be modified via procfs
 *
 * The following two allow fine tuning of the control loop and should not
 * be modified unless there is good understanding of their impact on the
 * stability of the controller.
 *
 * scale_cpus (default 150, range 50-1000)
 *   Small update_ms may lead to underestimate the number of CPUs
 *   simultaneously handling interrupts, and the opposite can happen
 *   with very large values. This parameter may help correct the value,
 *   though it is not recommended to modify the default unless there are
 *   very strong reasons.
 *
 * increase_divisor (default 8, range 8-128)
 *   This is base parameter used for multiplicative increase/decrease.
 */

#define MIN_SCALING_DIVISOR	8

struct irq_mod_params irq_mod_params ____cacheline_aligned = {
	.update_ns		= 5 * NSEC_PER_MSEC,
	.scale_cpus		= 150,
	.increase_divisor	= MIN_SCALING_DIVISOR,
	.seq			= SEQCNT_ZERO(irq_mod_params.seq),
};

/*
 * Accumulator for total interrupt and active CPUs, updated by all active
 * CPUs on each epoch (update_ns or more).
 * @total_intrs:	running count of total interrupts
 * @total_cpus:		running count of total active CPUs
 */
struct irq_mod_counters {
	atomic_t	total_intrs;
	atomic_t	total_cpus;
};
static struct irq_mod_counters irq_mod_counters;

DEFINE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

DEFINE_STATIC_KEY_FALSE(irq_moderation_enabled_key);

static DEFINE_MUTEX(swmod_mutex);

static void update_enable_key(void)
{
	lockdep_assert_held(&swmod_mutex);

	if (irq_mod_params.delay_ns != 0)
		static_branch_enable(&irq_moderation_enabled_key);
	else
		static_branch_disable(&irq_moderation_enabled_key);
}

/* Slow path functions for interrupt moderation. */

/*
 * Compute smoothed average between old and cur. 'steps' is used
 * to approximate applying the smoothing multiple times.
 */
static inline unsigned int smooth_avg(unsigned int old, unsigned int cur, unsigned int steps)
{
	const unsigned int smooth_factor = 64;
	u64 sum;

	steps = min(steps, smooth_factor - 1);
	sum = (u64)(smooth_factor - steps) * old + (u64)steps * cur;
	return div_u64(sum, smooth_factor);
}

/* Measure and assess time spent in hardirq. */
static inline bool hardirq_high(struct irq_mod_state *m, unsigned int hardirq_percent,
				u64 epoch_ns)
{
	bool above_threshold;
	u64 irqtime, cur;

	if (!IS_ENABLED(CONFIG_IRQ_TIME_ACCOUNTING))
		return false;

	cur = kcpustat_this_cpu->cpustat[CPUTIME_IRQ];
	irqtime = cur - m->last_irqtime;
	m->last_irqtime = cur;

	if (hardirq_percent == 0)
		return false;

	above_threshold = irqtime * 100 > epoch_ns * hardirq_percent;
	m->hardirq_high += above_threshold;
	return above_threshold;
}

/* Measure and assess total and per-CPU interrupt rates. */
static inline bool irqrate_high(struct irq_mod_state *m, unsigned int target_rate,
				unsigned int steps, u64 epoch_ns,
				unsigned int update_ns, unsigned int scale_cpus)
{
	unsigned int global_intr_rate, local_intr_rate, delta_intrs, tmp;
	bool local_rate_high, global_rate_high;
	u64 num_local, num_global, num_cpus;
	/* Use unsigned long to avoid overflow in intermediate results. */
	unsigned long active_cpus;
	u32 denom = epoch_ns;
	int shift = 0;

	num_local = (u64)m->intr_count * NSEC_PER_SEC;
	/* Scale denominator so we can avoid 64-bit division. */
	if (unlikely(epoch_ns > U32_MAX)) {
		shift = fls64(epoch_ns) - 32;
		denom = epoch_ns >> shift;
		num_local >>= shift;
	}

	local_intr_rate = div_u64(num_local, denom);

	/* Accumulate global counter and compute global interrupt rate. */
	tmp = atomic_add_return(m->intr_count, &irq_mod_counters.total_intrs);
	m->intr_count = 0;
	delta_intrs = tmp - m->last_total_intrs;
	m->last_total_intrs = tmp;
	num_global = (u64)delta_intrs * NSEC_PER_SEC;
	if (unlikely(shift))
		num_global >>= shift;
	global_intr_rate = div_u64(num_global, denom);

	/*
	 * Count how many CPUs handled interrupts in the last epoch, needed
	 * to determine the per-CPU target (target_rate / active_cpus).
	 * Each active CPU increments the global counter approximately every
	 * update_ns. Scale the value by (update_ns / epoch_ns) to get the
	 * correct value. Also apply rounding and make sure active_cpus > 0.
	 */
	tmp = atomic_add_return(1, &irq_mod_counters.total_cpus);
	active_cpus = tmp - m->last_total_cpus;
	m->last_total_cpus = tmp;
	num_cpus = (u64)active_cpus * update_ns + (epoch_ns / 2);
	if (unlikely(shift))
		num_cpus >>= shift;
	active_cpus = div_u64(num_cpus, denom);
	if (active_cpus < 1)
		active_cpus = 1;

	/* Compare with global and per-CPU targets. */
	global_rate_high = global_intr_rate > target_rate;

	/*
	 * Short epochs may lead to underestimate the number of active CPUs.
	 * Apply a scaling factor to compensate. This may make the controller
	 * a bit more aggressive but does not harm system throughput.
	 */
	local_rate_high = (u64)local_intr_rate * active_cpus *
			scale_cpus > (u64)target_rate * 100;

	/* Statistics. */
	m->global_intr_rate = smooth_avg(m->global_intr_rate, global_intr_rate, steps);
	m->local_intr_rate = smooth_avg(m->local_intr_rate, local_intr_rate, steps);
	m->scaled_cpu_count = smooth_avg(m->scaled_cpu_count, active_cpus * 256, steps);

	if (target_rate == 0)
		return false;

	m->local_irq_high += local_rate_high;
	m->global_irq_high += global_rate_high;

	/* Moderate on this CPU only if both global and local rates are high. */
	return global_rate_high && local_rate_high;
}

/* Periodic adjustment, called once per epoch. */
void irq_moderation_update_epoch(struct irq_mod_state *m, u64 epoch_ns)
{
	unsigned int hardirq_percent, target_rate, delay_ns, update_ns;
	unsigned int increase_divisor, scale_cpus;
	const unsigned int min_delay_ns = 500;
	bool above_target = false;
	unsigned int steps, seq;

	do {
		seq = read_seqcount_begin(&irq_mod_params.seq);
		hardirq_percent = READ_ONCE(irq_mod_params.hardirq_percent);
		target_rate = READ_ONCE(irq_mod_params.target_intr_rate);
		delay_ns = READ_ONCE(irq_mod_params.delay_ns);
		update_ns = READ_ONCE(irq_mod_params.update_ns);
		increase_divisor = READ_ONCE(irq_mod_params.increase_divisor);
		scale_cpus = READ_ONCE(irq_mod_params.scale_cpus);
	} while (read_seqcount_retry(&irq_mod_params.seq, seq));

	/*
	 * If one parameter changes, set the moderation delay to max, and rely
	 * on the adaptive mechanism to adjust it down if necessary.
	 * Otherwise the system may be stuck with an interrupt rate that is
	 * already below the threshold because of bus congestion (one of the
	 * problems that GSIM is trying to address), and the controller would
	 * have no signal react. Starting from a high value gives it a chance
	 * to converge if parameters allow it.
	 */
	if (seq != m->seq) {
		m->seq = seq;
		m->mod_ns = delay_ns;
		m->intr_count = 0;
		m->last_total_intrs = atomic_read(&irq_mod_counters.total_intrs);
		m->last_total_cpus = atomic_read(&irq_mod_counters.total_cpus);
		if (IS_ENABLED(CONFIG_IRQ_TIME_ACCOUNTING))
			m->last_irqtime = kcpustat_this_cpu->cpustat[CPUTIME_IRQ];
		return;
	}

	if (target_rate == 0 && hardirq_percent == 0) {
		/* Use fixed moderation delay. */
		m->mod_ns = delay_ns;
		m->global_intr_rate = 0;
		m->local_intr_rate = 0;
		m->scaled_cpu_count = 0;
		return;
	}

	/*
	 * The controller wants to scale the delay mod_ns by (1 + 1/D) every "update_ns".
	 * Since we operate every epoch_ns >= update_ns, the formula becomes
	 *   mod_ns = mod_ns * ((1 + 1/D) ** (epoch_ns / update_ns))
	 * which we approximate with "mod_ns = mod_ns * (1 + steps/D)"
	 * where "steps = epoch_ns / update_ns" clamped to a value < D.
	 */
	steps = (unsigned int)clamp_t(u64, div_u64(epoch_ns, update_ns),
				      1ULL, (u64)(MIN_SCALING_DIVISOR - 1u));

	if (irqrate_high(m, target_rate, steps, epoch_ns, update_ns, scale_cpus))
		above_target = true;

	if (hardirq_high(m, hardirq_percent, epoch_ns))
		above_target = true;

	/*
	 * Controller: adjust delay with exponential increase or decrease.
	 *
	 * Following standard practices, we increase fast (smaller divisor) to
	 * aggressively slow down when the interrupt rate goes up, but decrease
	 * slowly (larger divisor) to reduce the chance of load spikes as the
	 * delay goes down.
	 */
	if (above_target) {
		/* Make sure the value is large enough for the exponential to grow. */
		if (m->mod_ns < min_delay_ns)
			m->mod_ns = min_delay_ns;
		m->mod_ns += m->mod_ns * steps / increase_divisor;
		if (m->mod_ns > delay_ns)
			m->mod_ns = delay_ns;
	} else {
		m->mod_ns -= m->mod_ns * steps / (2 * increase_divisor);
		/* Round down to 0 values that are too small to bother. */
		if (m->mod_ns < min_delay_ns)
			m->mod_ns = 0;
	}
}

/* Actually start moderation. */
bool irq_moderation_do_start(struct irq_desc *desc, struct irq_mod_state *m)
{
	lockdep_assert_held(&desc->lock);

	if (!hrtimer_is_queued(&m->timer)) {
		const unsigned int min_delay_ns = 10000;
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
	 * Add to the timer list, set appropriate flags, and call
	 * __disable_irq() to prevent serving subsequent interrupts.
	 */
	m->enqueue++;
	list_add(&desc->swmod_state.swmod_node, &m->descs);
	/*
	 * Set IRQD_IRQ_INPROGRESS so that synchronize_irq() called during
	 * free_irq() will block until the timer drains this descriptor from
	 * the moderation list.
	 */
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS | IRQD_MODERATED);
	__disable_irq(desc);
	return true;
}

/*
 * struct var_info - target and limits for parameters
 * @ptr:	pointer to the value, NULL if not used.
 * @min:	minimum value allowed
 * @max:	maximum value allowed
 * @scale:	scale factor between procfs and internal.
 */
struct var_info {
	unsigned int	*ptr;
	unsigned int	min;
	unsigned int	max;
	unsigned int	scale;
};

/*
 * struct swmod_procfs_entry - description for procfs entries and parameter limits
 * @name:	name in procfs. If NULL, the entry is only for limit checks.
 * @wr:		write handler for procfs. NULL if readonly
 * @rd:		read handler for procfs.
 * @var:	variable address and limits, if used.
 */
struct swmod_procfs_entry {
	const char	*name;
	ssize_t		(*wr)(struct var_info *n, const char __user *s, size_t count);
	void		(*rd)(struct seq_file *p);
	struct var_info	var;
};

static void write_param(unsigned int *ptr, unsigned int value)
{
	unsigned long flags;

	local_irq_save(flags);
	write_seqcount_begin(&irq_mod_params.seq);
	WRITE_ONCE(*ptr, value);
	write_seqcount_end(&irq_mod_params.seq);
	local_irq_restore(flags);
}

static ssize_t swmod_wr(struct var_info *v, const char __user *s, size_t count)
{
	unsigned int value;
	int ret;

	lockdep_assert_held(&swmod_mutex);

	ret = kstrtouint_from_user(s, count, 0, &value);
	if (ret)
		return ret;
	if (value < v->min || value > v->max)
		return -ERANGE;
	write_param(v->ptr, value * v->scale);

	return count;
}

static void swmod_rd(struct seq_file *p)
{
	struct swmod_procfs_entry *n = p->private;

	seq_printf(p, "%u\n", *n->var.ptr / n->var.scale);
}

static ssize_t swmod_wr_delay(struct var_info *v, const char __user *s, size_t count)
{
	ssize_t ret = swmod_wr(v, s, count);

	if (ret >= 0)
		update_enable_key();
	return ret;
}

#define HEAD_FMT "%5s  %8s  %10s  %4s  %8s  %11s  %11s  %11s  %11s  %11s\n"
#define BODY_FMT "%5u  %8u  %10u  %4u  %8u  %11u  %11u  %11u  %11u  %11u\n"

/* Print statistics */
static void rd_stats(struct seq_file *p)
{
	unsigned int delay_ns = READ_ONCE(irq_mod_params.delay_ns);
	unsigned long global_intr_rate = 0, global_irq_high = 0;
	unsigned long local_irq_high = 0, hardirq_high = 0;
	int recent_epoch_limit, cpu, active_cpus = 0;

	if (delay_ns == 0)
		return;
	seq_printf(p, HEAD_FMT,
		   "# CPU", "irq/s", "loc_irq/s", "cpus", "delay_ns",
		   "irq_hi", "loc_irq_hi", "hardirq_hi", "timer_set",
		   "enqueue");

	/*
	 * Accumulate/print only entries updated within ~20-30s. The high 32 bits
	 * of timestamps give ~4s resolution, so we can use them without the need
	 * for 64bit atomics (because epoch_start_ns is updated concurrently).
	 */
	recent_epoch_limit = (ktime_get_ns() - 20ULL * NSEC_PER_SEC) >> 32;

	for_each_possible_cpu(cpu) {
		/* Copy statistics, will only use some unsigned int values; races ok. */
		struct irq_mod_state cur = data_race(*per_cpu_ptr(&irq_mod_state, cpu));

		if (cur.epoch_start_ns && (int)(cur.epoch_start_ns >> 32) >= recent_epoch_limit) {
			/* Recent entry, accumulate in global rate. */
			active_cpus++;
			global_intr_rate += cur.global_intr_rate;
		} else {
			/* Stale entries, print as 0. */
			cur.global_intr_rate = 0;
			cur.local_intr_rate = 0;
			cur.scaled_cpu_count = 0;
			cur.mod_ns = 0;
		}

		global_irq_high += cur.global_irq_high;
		local_irq_high += cur.local_irq_high;
		hardirq_high += cur.hardirq_high;

		seq_printf(p, BODY_FMT,
			   cpu,
			   cur.global_intr_rate,
			   cur.local_intr_rate,
			   (cur.scaled_cpu_count + 128) / 256,
			   cur.mod_ns,
			   cur.global_irq_high,
			   cur.local_irq_high,
			   cur.hardirq_high,
			   cur.timer_set,
			   cur.enqueue);
	}

	seq_printf(p, "\n"
		   "delay_us             %lu\n"
		   "target_intr_rate     %u\n"
		   "hardirq_percent      %u\n"
		   "update_ms            %ld\n"
		   "scale_cpus           %u\n",
		   delay_ns / NSEC_PER_USEC,
		   READ_ONCE(irq_mod_params.target_intr_rate),
		   READ_ONCE(irq_mod_params.hardirq_percent),
		   READ_ONCE(irq_mod_params.update_ns) / NSEC_PER_MSEC,
		   READ_ONCE(irq_mod_params.scale_cpus));

	seq_printf(p,
		   "intr_rate            %lu\n"
		   "irq_high             %lu\n"
		   "my_irq_high          %lu\n"
		   "hardirq_percent_high %lu\n"
		   "total_interrupts     %u\n"
		   "total_cpus           %u\n",
		   active_cpus ? global_intr_rate / active_cpus : 0,
		   global_irq_high, local_irq_high, hardirq_high,
		   atomic_read(&irq_mod_counters.total_intrs),
		   atomic_read(&irq_mod_counters.total_cpus));
}

static int param_show(struct seq_file *p, void *v)
{
	struct swmod_procfs_entry *n = p->private;

	n->rd(p);
	return 0;
}

static int param_open(struct inode *inode, struct file *file)
{
	return single_open(file, param_show, pde_data(inode));
}

static ssize_t param_write(struct file *f, const char __user *buf, size_t count, loff_t *ppos)
{
	struct swmod_procfs_entry *n = (struct swmod_procfs_entry *)pde_data(file_inode(f));
	ssize_t ret;

	if (!n->wr)
		return -EINVAL;
	mutex_lock(&swmod_mutex);
	ret = n->wr(&n->var, buf, count);
	mutex_unlock(&swmod_mutex);
	return ret;
}

static const struct proc_ops param_ops = {
	.proc_open	= param_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= param_write,
};

/* Handlers for /proc/irq/NN/allow_sw_moderation */
static int allow_flag_show(struct seq_file *p, void *v)
{
	struct irq_desc *desc = irq_to_desc((long)p->private);

	if (!desc)
		return -ENODEV;

	seq_puts(p, irq_settings_moderatable(desc) ? "on\n" : "off\n");
	return 0;
}


static ssize_t allow_flag_write(struct file *f, const char __user *buf, size_t count, loff_t *ppos)
{
	struct irq_desc *desc = irq_to_desc((long)pde_data(file_inode(f)));
	bool allow;
	int ret;

	if (!desc)
		return -ENODEV;

	ret = kstrtobool_from_user(buf, count, &allow);

	if (!ret) {
		guard(raw_spinlock_irq)(&desc->lock);
		ret = irq_moderation_allow(desc, allow);
	}
	return ret ? : count;
}

static int allow_flag_open(struct inode *inode, struct file *file)
{
	return single_open(file, allow_flag_show, pde_data(inode));
}

static const struct proc_ops allow_flag_ops = {
	.proc_open	= allow_flag_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= allow_flag_write,
};

void irq_moderation_procfs_add(struct irq_desc *desc, umode_t umode)
{
	if (!irq_moderation_supported(desc))
		return;
	proc_create_data("allow_sw_moderation", umode, desc->dir,
			 &allow_flag_ops, (void *)(long)desc->irq_data.irq);
}

void irq_moderation_procfs_remove(struct irq_desc *desc)
{
	remove_proc_entry("allow_sw_moderation", desc->dir);
}

static void clean_moderation_state(struct irq_desc *desc)
{
	/*
	 * Clearing IRQD_IRQ_INPROGRESS allows synchronize_irq() to complete,
	 * signaling that GSIM teardown for this descriptor is finished.
	 */
	irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS | IRQD_MODERATED);
	/* Only enable if action is set, protect against concurrent free_irq(). */
	if (desc->action)
		__enable_irq(desc);
}

/* Used on timer expiration or CPU shutdown. */
static void drain_desc_list(struct irq_mod_state *m)
{
	struct irq_desc *desc, *next;

	/* Remove from list and enable interrupts back. */
	list_for_each_entry_safe(desc, next, &m->descs, swmod_state.swmod_node) {
		guard(raw_spinlock)(&desc->lock);
		list_del_init(&desc->swmod_state.swmod_node);
		clean_moderation_state(desc);
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
static int cpu_setup_cb(unsigned int cpu)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	hrtimer_setup(&m->timer, timer_callback, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	INIT_LIST_HEAD(&m->descs);
	/* Ensure initialization is visible before setting the flag. */
	smp_store_release(&m->initialized, true);
	m->moderation_allowed = true;
	return 0;
}

/*
 * Hotplug callback for shutdown.
 * Mark the CPU as offline for moderation, and drain the list of masked
 * interrupts. Any subsequent interrupt on this CPU will not be
 * moderated, but they will be on the new target.
 */
static int cpu_remove_cb(unsigned int cpu)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	/* Protect the desc list, interrupts could modify it. */
	scoped_guard(irqsave) {
		m->moderation_allowed = false;
		drain_desc_list(m);
	}
	/* Run hrtimer_cancel() outside hardirq/IPI context. */
	hrtimer_cancel(&m->timer);
	/* Ensure state is visible before clearing the flag. */
	smp_store_release(&m->initialized, false);
	return 0;
}

static void mod_pm_prepare_cb(void *arg)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	/* Called via IPI so local interrupts are disabled. */
	if (mod_state_initialized(m)) {
		m->moderation_allowed = false;
		drain_desc_list(m);
	}
}

static void mod_pm_resume_cb(void *arg)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	/*
	 * The hrtimer and list head are already initialized (either at boot
	 * or during hotplug CPU online). We must not re-initialize them here
	 * as they might already be active if devices resumed and fired
	 * interrupts before this notifier ran.
	 */
	if (mod_state_initialized(m))
		m->moderation_allowed = true;
}

static int mod_pm_notifier_cb(struct notifier_block *nb, unsigned long event, void *unused)
{
	int cpu;

	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		on_each_cpu(mod_pm_prepare_cb, NULL, 1);
		/* Run hrtimer_cancel() outside hardirq/IPI context. */
		for_each_online_cpu(cpu) {
			struct irq_mod_state *m = per_cpu_ptr(&irq_mod_state, cpu);

			if (mod_state_initialized(m))
				hrtimer_cancel(&m->timer);
		}
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		on_each_cpu(mod_pm_resume_cb, NULL, 1);
		break;
	}
	return NOTIFY_OK;
}

struct notifier_block mod_nb = {
	.notifier_call	= mod_pm_notifier_cb,
	.priority	= 100,
};

/* Helper to initialize the struct var_info. */
#define SET_VAR(_ptr, _min, _max, _scale)					\
	{ .ptr = (_ptr), .min = (_min), .max = (_max), .scale = (_scale), }

static struct swmod_procfs_entry procfs_entries[] = {
	{
		.name	= "delay_us",
		.wr	= swmod_wr_delay,
		.rd	= swmod_rd,
		.var	= SET_VAR(&irq_mod_params.delay_ns, 0, 500, NSEC_PER_USEC),
	},
	{
		.name	= "target_intr_rate",
		.wr	= swmod_wr,
		.rd	= swmod_rd,
		.var	= SET_VAR(&irq_mod_params.target_intr_rate, 0, 50000000, 1),
	},
	{
		.name	= "hardirq_percent",
		.wr	= swmod_wr,
		.rd	= swmod_rd,
		.var	= SET_VAR(&irq_mod_params.hardirq_percent, 0, 100, 1),
	},
	{
		.name	= "update_ms",
		.wr	= swmod_wr,
		.rd	= swmod_rd,
		.var	= SET_VAR(&irq_mod_params.update_ns, 1, 100, NSEC_PER_MSEC),
	},
	{
		.name = "stats",
		.rd = rd_stats,
	},
	/* The next parameters have no procfs entries, only range validation. */
	{
		.var	= SET_VAR(&irq_mod_params.increase_divisor, MIN_SCALING_DIVISOR, 128, 1),
	},
	{
		.var	= SET_VAR(&irq_mod_params.scale_cpus, 50, 1000, 1),
	},
};

static int __init init_irq_moderation(void)
{
	struct proc_dir_entry *dir;
	unsigned long flags;
	int cpuhp_state;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(procfs_entries); i++) {
		struct var_info *v = &procfs_entries[i].var;

		if (!v->ptr)
			continue;
		if (*v->ptr >= v->min * v->scale && *v->ptr <= v->max * v->scale)
			continue;
		pr_err("%s: Parameter %s: value %u out of bounds [%u,%u]\n",
		       __func__, procfs_entries[i].name ? : "no-name",
		       *v->ptr / v->scale, v->min, v->max);
		return -ERANGE;
	}

	cpuhp_state = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "sw_moderation",
					cpu_setup_cb, cpu_remove_cb);
	if (cpuhp_state < 0) {
		pr_err("%s: Failed to setup hotplug notifier\n", __func__);
		return cpuhp_state;
	}

	ret = register_pm_notifier(&mod_nb);
	if (ret < 0) {
		pr_err("%s: Failed to register pm notifier\n", __func__);
		goto cleanup;
	}

	/* Safe because /proc/irq is created earlier, in kernel_init_freeable(). */
	dir = proc_mkdir("irq/sw_moderation", NULL);
	if (!dir) {
		pr_err("%s: Failed to create procfs directory\n", __func__);
		goto cleanup_1;
	}
	for (i = 0; i < ARRAY_SIZE(procfs_entries); i++) {
		struct swmod_procfs_entry *n = &procfs_entries[i];

		if (!n->name || proc_create_data(n->name, n->wr ? 0644 : 0444, dir, &param_ops, n))
			continue;
		pr_err("%s: Failed to create procfs entry %s\n", __func__, n->name);
		for (i--; i >= 0; i--) {
			n = &procfs_entries[i];
			if (n->name)
				remove_proc_entry(n->name, dir);
		}
		remove_proc_entry("irq/sw_moderation", NULL);
		goto cleanup_1;
	}

	/* Increment sequence counter so per-CPU m->seq (0) mismatches on epoch 1 */
	local_irq_save(flags);
	write_seqcount_begin(&irq_mod_params.seq);
	write_seqcount_end(&irq_mod_params.seq);
	local_irq_restore(flags);

	/* Enable if the defaults require it. */
	/* Acquire swmod_mutex to satisfy lockdep assertions. */
	mutex_lock(&swmod_mutex);
	update_enable_key();
	mutex_unlock(&swmod_mutex);
	return 0;

cleanup_1:
	ret = -ENOMEM;
	unregister_pm_notifier(&mod_nb);

cleanup:
	cpuhp_remove_state(cpuhp_state);
	return ret;
}
device_initcall(init_irq_moderation);

bool irq_moderation_supported(struct irq_desc *desc)
{
	struct irq_data *irqd = &desc->irq_data;
	struct irq_chip *chip = irqd->chip;

	/* GSIM does not support shared interrupts */
	if (desc->action && desc->action->next)
		return false;

	if (desc->istate & IRQS_ONESHOT)
		return false;
	if (irqd_is_level_type(irqd))
		return false;
	if (!irqd_is_single_target(irqd))
		return false;
	if (chip->irq_bus_lock || chip->irq_bus_sync_unlock)
		return false;
	if (!chip->irq_mask || !chip->irq_unmask)
		return false;
	if (desc->handle_irq != handle_edge_irq && desc->handle_irq != handle_fasteoi_irq)
		return false;
	return true;
}

int irq_moderation_allow(struct irq_desc *desc, bool allow)
{
	lockdep_assert_held(&desc->lock);

	if (!allow) {
		irq_settings_clr_moderatable(desc);
		return 0;
	}

	if (!irq_moderation_supported(desc)) {
		irq_settings_clr_moderatable(desc);
		return -EOPNOTSUPP;
	}

	irq_settings_set_moderatable(desc);
	return 0;
}
