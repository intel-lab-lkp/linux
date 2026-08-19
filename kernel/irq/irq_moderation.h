/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

/*
 * Copyright (C) 2025-2026 Google LLC
 *
 * Common data structures for Global Software Interrupt Moderation, GSIM
 */

#ifndef _LINUX_IRQ_MODERATION_H
#define _LINUX_IRQ_MODERATION_H

#ifdef CONFIG_IRQ_SW_MODERATION

#include <linux/hrtimer.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel.h>
#include <linux/seqlock.h>

/**
 * struct irq_mod_params - configuration parameters
 * @delay_ns:		maximum delay
 * @target_intr_rate:	target maximum interrupt rate
 * @hardirq_percent:	target maximum hardirq percentage
 * @update_ns:		how often to update delay/rate/fraction (epoch duration)
 * @increase_divisor:	constant for multiplicative increase/decrease of delay
 * @scale_cpus:		(percent) scale factor to estimate active CPUs
 * @seq:		incremented every time parameters change
 */
struct irq_mod_params {
	unsigned int	delay_ns;
	unsigned int	target_intr_rate;
	unsigned int	hardirq_percent;
	unsigned int	update_ns;
	unsigned int	increase_divisor;
	unsigned int	scale_cpus;
	seqcount_t	seq;
};

extern struct irq_mod_params irq_mod_params;

/**
 * struct irq_mod_state - per-CPU moderation state
 *
 * Used on every interrupt:
 * @timer:		moderation timer
 * @initialized:	true if hrtimer and list head are initialized
 * @moderation_allowed:	per-CPU flag, toggled during hotplug/suspend events
 * @sleep_ns:		accumulated time for actual delay
 * @mod_ns:		dynamically computed moderation delay
 * @intr_count:		interrupt counter
 * @epoch_start_ns:	start time of current epoch
 *
 * Used once per moderation delay per interrupt source:
 * @descs:		list of	moderated irq_desc on this CPU
 * @enqueue:		how many enqueue on the list
 *
 * Used once per epoch:
 * @seq:		latest seq from irq_mod_info
 * @last_total_intrs:	from irq_mod_info
 * @last_total_cpus:	from irq_mod_info
 * @last_irqtime:	from cpustat[CPUTIME_IRQ]
 *
 * Statistics
 * @global_intr_rate:	smoothed global interrupt rate
 * @local_intr_rate:	smoothed interrupt rate for this CPU
 * @timer_set:		how many timer_set calls
 * @scaled_cpu_count:	smoothed CPU count (scaled)
 * @global_irq_high:	how many times global irq rate was above threshold
 * @local_irq_high:	how many times local irq rate was above threshold
 * @hardirq_high:	how many times local hardirq_percent was above threshold
 */
struct irq_mod_state {
	struct hrtimer		timer;
	bool			initialized;
	bool			moderation_allowed;
	unsigned int		sleep_ns;
	unsigned int		mod_ns;
	unsigned int		intr_count;
	u64			epoch_start_ns;
	struct list_head	descs;
	unsigned int		enqueue;
	unsigned int		seq;
	unsigned int		last_total_intrs;
	unsigned int		last_total_cpus;
	u64			last_irqtime;
	unsigned int		global_intr_rate;
	unsigned int		local_intr_rate;
	unsigned int		timer_set;
	unsigned int		scaled_cpu_count;
	unsigned int		global_irq_high;
	unsigned int		local_irq_high;
	unsigned int		hardirq_high;
};

DECLARE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

static inline bool mod_state_initialized(struct irq_mod_state *m)
{
	/*
	 * There is no public API in the hrtimer or list subsystems to check
	 * if they are initialized. We use this flag to avoid dereferencing
	 * uninitialized pointers during CPU hotplug/suspend races.
	 */
	return smp_load_acquire(&m->initialized);
}

extern struct static_key_false irq_moderation_enabled_key;

bool irq_moderation_do_start(struct irq_desc *desc, struct irq_mod_state *m);
void irq_moderation_update_epoch(struct irq_mod_state *m, u64 epoch_ns);

static inline void check_epoch(struct irq_mod_state *m)
{
	const unsigned int slack_ns = 5000;
	u64 now, epoch_ns;

	/* Don't check too often, fetching time is moderately expensive. */
	if ((m->intr_count & 0xf) != 0)
		return;
	now = ktime_get_ns();
	epoch_ns = now - m->epoch_start_ns;

	/* Run approximately every update_ns, a little bit early is ok. */
	if (epoch_ns < READ_ONCE(irq_mod_params.update_ns) - slack_ns)
		return;
	WRITE_ONCE(m->epoch_start_ns, now);
	/* Do the expensive processing. */
	irq_moderation_update_epoch(m, epoch_ns);
}

/*
 * Call after running the handler, with lock held. If this source should be
 * moderated, disable it, add to the timer list for this CPU and return true,
 * and exit from handle_*_irq() without processing IRQS_PENDING, because
 * that will happen when the moderation timer fires and calls __enable_irq().
 */
static inline bool irq_start_moderation(struct irq_desc *desc)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	if (static_branch_unlikely(&irq_moderation_enabled_key) &&
	    irq_settings_moderatable(desc) &&
	    m->moderation_allowed) {
		m->intr_count++;
		check_epoch(m);
		return irq_moderation_do_start(desc, m);
	}
	return false;
}

#else /* CONFIG_IRQ_SW_MODERATION */
static inline bool irq_start_moderation(struct irq_desc *desc) { return false; }
#endif /* CONFIG_IRQ_SW_MODERATION */

#endif /* _LINUX_IRQ_MODERATION_H */
