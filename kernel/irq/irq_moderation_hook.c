// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel_stat.h>

#include "internals.h"
#include "irq_moderation_hook.h"

/* Slow path functions for interrupt moderation. */

static inline u32 smooth_avg(u32 old, u32 cur, u32 steps)
{
	const u32 smooth_factor = 64;

	steps = min(steps, smooth_factor - 1);
	return ((smooth_factor - steps) * old + steps * cur) / smooth_factor;
}

/* Measure and assess time spent in hardirq. */
static inline bool hardirq_high(struct irq_mod_state *ms, u32 hardirq_percent)
{
	bool above_threshold = false;

	if (IS_ENABLED(CONFIG_IRQ_TIME_ACCOUNTING)) {
		u64 irqtime, cur = kcpustat_this_cpu->cpustat[CPUTIME_IRQ];

		irqtime = cur - ms->last_irqtime;
		ms->last_irqtime = cur;

		above_threshold = irqtime * 100 > (u64)ms->epoch_ns * hardirq_percent;
		ms->hardirq_high += above_threshold;
	}
	return above_threshold;
}

/* Measure and assess total and per-CPU interrupt rates. */
static inline bool irqrate_high(struct irq_mod_state *ms, u32 target_rate, u32 steps)
{
	u32 intr_rate, my_intr_rate, delta_intrs, active_cpus, tmp;
	bool my_rate_high, global_rate_high;

	my_intr_rate = ((u64)ms->intr_count * NSEC_PER_SEC) / ms->epoch_ns;

	/* Accumulate global counter and compute global interrupt rate. */
	tmp = atomic_add_return(ms->intr_count, &irq_mod_info.total_intrs);
	ms->intr_count = 1;
	delta_intrs = tmp - ms->last_total_intrs;
	ms->last_total_intrs = tmp;
	intr_rate = ((u64)delta_intrs * NSEC_PER_SEC) / ms->epoch_ns;

	/*
	 * Count how many CPUs handled interrupts in the last epoch, needed
	 * to determine the per-CPU target (target_rate / active_cpus).
	 * Each active CPU increments the global counter approximately every
	 * update_ns. Scale the value by (update_ns / ms->epoch_ns) to get the
	 * correct value. Also apply rounding and make sure active_cpus > 0.
	 */
	tmp = atomic_add_return(1, &irq_mod_info.total_cpus);
	active_cpus = tmp - ms->last_total_cpus;
	ms->last_total_cpus = tmp;
	active_cpus = (active_cpus * ms->update_ns + ms->epoch_ns / 2) / ms->epoch_ns;
	if (active_cpus < 1)
		active_cpus = 1;

	/* Compare with global and per-CPU targets. */
	global_rate_high = intr_rate > target_rate;

	/*
	 * Short epochs may lead to underestimate the number of active CPUs.
	 * Apply a scaling factor to compensate. This may make the controller
	 * a bit more aggressive but does not harm system throughput.
	 */
	my_rate_high = my_intr_rate * active_cpus * irq_mod_info.scale_cpus > target_rate * 100;

	/* Statistics. */
	ms->intr_rate = smooth_avg(ms->intr_rate, intr_rate, steps);
	ms->my_intr_rate = smooth_avg(ms->my_intr_rate, my_intr_rate, steps);
	ms->scaled_cpu_count = smooth_avg(ms->scaled_cpu_count, active_cpus * 256, steps);
	ms->my_irq_high += my_rate_high;
	ms->irq_high += global_rate_high;

	/* Moderate on this CPU only if both global and local rates are high. */
	return global_rate_high && my_rate_high;
}

/* Periodic adjustment, called once per epoch. */
void irq_moderation_update_epoch(struct irq_mod_state *ms)
{
	const u32 hardirq_percent = READ_ONCE(irq_mod_info.hardirq_percent);
	const u32 target_rate = READ_ONCE(irq_mod_info.target_intr_rate);
	const u32 min_delay_ns = 500;
	bool above_target = false;
	u32 version;
	u32 steps;

	/* Fetch updated parameters. */
	while ((version = READ_ONCE(irq_mod_info.version)) != ms->version) {
		ms->update_ns = READ_ONCE(irq_mod_info.update_ms) * NSEC_PER_MSEC;
		ms->mod_ns = READ_ONCE(irq_mod_info.delay_us) * NSEC_PER_USEC;
		ms->delay_ns = ms->mod_ns;
		ms->version = version;
	}

	if (target_rate == 0 && hardirq_percent == 0) {
		/* Use fixed moderation delay. */
		ms->mod_ns = ms->delay_ns;
		ms->intr_rate = 0;
		ms->my_intr_rate = 0;
		ms->scaled_cpu_count = 0;
		return;
	}

	/*
	 * To scale values X by a factor (1 +/- 1/F) every "update_ns" we do
	 * 	X := X * (1 +/- 1/F)
	 * If the interval is N times longer, applying the formula N times gives
	 * 	X := X * ((1 +/- 1/F) ** N)
	 * We don't want to deal floating point or exponentials, and we cap N
	 * to some small value < F . This leads to an approximated formula
	 * 	X := X * (1 +/- N/F)
	 * The variable steps below is the number N of steps.
	 */
	steps = clamp(ms->epoch_ns / ms->update_ns, 1u, MIN_SCALING_FACTOR - 1u);

	if (target_rate > 0 && irqrate_high(ms, target_rate, steps))
		above_target = true;

	if (hardirq_percent > 0 && hardirq_high(ms, hardirq_percent))
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
		if (ms->mod_ns < min_delay_ns)
			ms->mod_ns = min_delay_ns;
		ms->mod_ns += ms->mod_ns * steps / increase_factor;
		if (ms->mod_ns > ms->delay_ns)
			ms->mod_ns = ms->delay_ns;
	} else {
		const u32 decrease_factor = 2 * READ_ONCE(irq_mod_info.increase_factor);

		ms->mod_ns -= ms->mod_ns * steps / decrease_factor;
		/* Round down to 0 values that are too small to bother. */
		if (ms->mod_ns < min_delay_ns)
			ms->mod_ns = 0;
	}
}
