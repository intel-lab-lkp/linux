/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#ifndef _LINUX_IRQ_MODERATION_H
#define _LINUX_IRQ_MODERATION_H

/*
 * Platform wide software interrupt moderation, see
 * Documentation/core-api/irq/irq-moderation.rst
 */

#include <linux/hrtimer.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel.h>

#ifdef CONFIG_IRQ_SOFT_MODERATION

/**
 * struct irq_mod_info - global configuration parameters and state
 * @total_intrs:	running count updated every update_ms
 * @total_cpus:		as above, active CPUs in this interval
 * @procfs_write_ns:	last write to /proc/irq/soft_moderation
 * @delay_us:		fixed delay, or maximum for adaptive
 * @target_irq_rate:	target maximum interrupt rate
 * @hardirq_percent:	target maximum hardirq percentage
 * @timer_rounds:	how many timer polls once moderation fires
 * @update_ms:		how often to update delay/rate/fraction
 * @scale_cpus:		(percent) scale factor to estimate active CPUs
 * @count_timer_calls:	count timer calls for irq limits
 * @count_msi_calls:	count calls from posted_msi for irq limits
 * @decay_factor:	smoothing factor for the control loop, keep at 16
 * @grow_factor:	smoothing factor for the control loop, keep it at 8
 */
struct irq_mod_info {
	/* These fields are written to by all CPUs */
	____cacheline_aligned
	atomic_long_t total_intrs;
	atomic_long_t total_cpus;

	/* These are mostly read (frequently), so use a different cacheline */
	____cacheline_aligned
	u64 procfs_write_ns;
	uint delay_us;
	uint target_irq_rate;
	uint hardirq_percent;
	uint timer_rounds;
	uint update_ms;
	uint scale_cpus;
	uint count_timer_calls;
	uint count_msi_calls;
	uint decay_factor;
	uint grow_factor;
	uint pad[];
};

extern struct irq_mod_info irq_mod_info;

/**
 * struct irq_mod_state - per-CPU moderation state
 *
 * @timer:		moderation timer
 * @descs:		list of	moderated irq_desc on this CPU
 *
 * Counters on last time we updated moderation delay
 * @last_ns:		time of last update
 * @last_irqtime:	from cpustat[CPUTIME_IRQ]
 * @last_total_irqs:	from irq_mod_info
 * @last_total_cpus:	from irq_mod_info
 *
 * Local info to control hooks and timer callbacks
 * @dont_count:		do not count this interrupt
 * @in_posted_msi:	don't suppress handle_irq, set in posted_msi handler
 * @kick_posted_msi:	kick posted_msi from the timer callback
 * @rounds_left:	how many rounds left for timer callbacks
 *
 * @irq_count:		irqs in the last cycle, signed as we also decrement
 * @update_ns:		fetched from irq_mod_info
 * @delay_ns:		fetched from irq_mod_info
 * @mod_ns:		current moderation delay, recomputed every update_ms
 * @sleep_ns:		accumulated time for actual delay
 *
 * Statistics
 * @irq_rate:		smoothed global irq rate
 * @my_irq_rate:	smoothed irq rate for this CPU
 * @scaled_cpu_count:	smoothed CPU count (scaled)
 * @scaled_src_count:	smoothed count of irq sources (scaled)
 * @irq_high:		how many times global irq above threshold
 * @my_irq_high:	how many times local irq above threshold
 * @hardirq_high:	how many times local hardirq_percent above threshold
 * @timer_set:		how many timer_set calls
 * @timer_fire:		how many timer_fire, must match timer_set in timer callback
 * @disable_irq:	how many disable_irq calls
 * @enable_irq:		how many enable_irq, must match disable_irq in timer callback
 * @timer_calls:	how many handler calls from timer interrupt
 * @from_posted_msi:	how many calls from posted_msi handler
 * @stray_irq:		how many stray interrupts
 */
struct irq_mod_state {
	struct hrtimer timer;
	struct list_head descs;

	/* Counters on last time we updated moderation delay */
	u64 last_ns;
	u64 last_irqtime;
	u64 last_total_irqs;
	u64 last_total_cpus;

	bool dont_count;
	bool in_posted_msi;
	bool kick_posted_msi;
	u8 rounds_left;

	u32 irq_count;
	u32 update_ns;
	u32 delay_ns;
	u32 mod_ns;
	u32 sleep_ns;

	/* Statistics */
	u32 irq_rate;
	u32 my_irq_rate;
	u32 scaled_cpu_count;
	u32 scaled_src_count;
	u32 irq_high;
	u32 my_irq_high;
	u32 hardirq_high;
	u32 timer_set;
	u32 timer_fire;
	u32 disable_irq;
	u32 enable_irq;
	u32 timer_calls;
	u32 from_posted_msi;
	u32 stray_irq;
	int pad[] ____cacheline_aligned;
};

DECLARE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

extern struct static_key_false irq_moderation_enabled_key;

void __irq_moderation_adjust_delay(struct irq_mod_state *ms, u64 delta_time);

/* Called on each interrupt for adaptive moderation delay adjustment. */
static inline void irq_moderation_adjust_delay(struct irq_mod_state *ms)
{
	u64 now, delta_time;

	ms->irq_count++;
	/* ktime_get_ns() is expensive, don't do too often */
	if (ms->irq_count & 0xf)
		return;
	now = ktime_get_ns();
	delta_time = now - ms->last_ns;

	/* Run approximately every update_ns, a little bit early is ok. */
	if (delta_time < ms->update_ns - 5000)
		return;

	ms->update_ns = READ_ONCE(irq_mod_info.update_ms) * NSEC_PER_MSEC;
	ms->delay_ns = READ_ONCE(irq_mod_info.delay_us) * NSEC_PER_USEC;

	/* If config changed, restart from the highest delay. */
	if (ktime_compare(irq_mod_info.procfs_write_ns, ms->last_ns) > 0)
		ms->mod_ns = ms->delay_ns;

	ms->last_ns = now;
	/* Do the expensive processing */
	__irq_moderation_adjust_delay(ms, delta_time);
}

/* Return true if timer is active or delay is large enough to require moderation */
static inline bool irq_moderation_needed(struct irq_mod_state *ms)
{
	const u32 min_delay_ns = 10000;

	if (!hrtimer_is_queued(&ms->timer)) {
		/* accumulate sleep time, no moderation if too small */
		ms->sleep_ns += ms->mod_ns;
		if (ms->sleep_ns < min_delay_ns)
			return false;
	}
	return true;
}

void disable_irq_nosync(unsigned int irq);

/*
 * Use in handle_irq_event() before calling the handler. Decide whether this
 * desc should be moderated, and in case disable the irq and add the desc to
 * the list for this CPU.
 */
static inline void irq_moderation_hook(struct irq_desc *desc)
{
	struct irq_mod_state *ms = this_cpu_ptr(&irq_mod_state);

	if (!static_branch_unlikely(&irq_moderation_enabled_key))
		return;

	if (!READ_ONCE(desc->mod.enable))
		return;

	irq_moderation_adjust_delay(ms);

	if (!list_empty(&desc->mod.ms_node)) {
		/*
		 * Very unlikely, stray interrupt while the desc is moderated.
		 * We cannot ignore it or we may miss events, but do count it.
		 */
		ms->stray_irq++;
		return;
	}

	if (!irq_moderation_needed(ms))
		return;

	/* Add to list of moderated desc on this CPU */
	list_add(&desc->mod.ms_node, &ms->descs);
	/*
	 * Disable the irq. This will also cause irq_can_handle() return false
	 * (through irq_can_handle_actions()), and that will prevent a handler
	 * instance to be run again while the descriptor is being moderated.
	 *
	 * irq_moderation_epilogue() will then start the timer if needed.
	 */
	ms->disable_irq++;
	disable_irq_nosync(desc->irq_data.irq);
}

static inline void irq_moderation_start_timer(struct irq_mod_state *ms)
{
	ms->timer_set++;
	ms->rounds_left = READ_ONCE(irq_mod_info.timer_rounds) + 1;
	hrtimer_start_range_ns(&ms->timer, ns_to_ktime(ms->sleep_ns),
			       /*range*/2000, HRTIMER_MODE_REL_PINNED_HARD);
}

/* After the handler, if desc is moderated, make sure the timer is active. */
static inline void irq_moderation_epilogue(const struct irq_desc *desc)
{
	struct irq_mod_state *ms = this_cpu_ptr(&irq_mod_state);

	if (!list_empty(&desc->mod.ms_node) && !hrtimer_is_queued(&ms->timer))
		irq_moderation_start_timer(ms);
}

void irq_moderation_procfs_add(struct irq_desc *desc, umode_t umode);
void irq_moderation_procfs_remove(struct irq_desc *desc);

#else /* CONFIG_IRQ_SOFT_MODERATION */

static inline void irq_moderation_hook(struct irq_desc *desc) {}
static inline void irq_moderation_epilogue(const struct irq_desc *desc) {}

static inline void irq_moderation_procfs_add(struct irq_desc *desc, umode_t umode) {}
static inline void irq_moderation_procfs_remove(struct irq_desc *desc) {}

#endif /* !CONFIG_IRQ_SOFT_MODERATION */

#endif /* _LINUX_IRQ_MODERATION_H */
