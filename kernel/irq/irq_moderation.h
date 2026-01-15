/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#ifndef _LINUX_IRQ_MODERATION_H
#define _LINUX_IRQ_MODERATION_H

#ifdef CONFIG_IRQ_SW_MODERATION
/* Common data structures for Global Software Interrupt Moderation, GSIM */

#include <linux/hrtimer.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel.h>

/**
 * struct irq_mod_info - global configuration parameters and state
 * @delay_us:		maximum delay
 * @update_ms:		how often to update delay (epoch duration)
 */
struct irq_mod_info {
	u32		delay_us;
	u32		update_ms;
	u32		params_end[];
};

extern struct irq_mod_info irq_mod_info;

/**
 * struct irq_mod_state - per-CPU moderation state
 *
 * Used on every interrupt:
 * @timer:		moderation timer
 * @moderation_allowed:	per-CPU flag, toggled during hotplug/suspend events
 * @intr_count:		interrupts in the last epoch
 * @sleep_ns:		accumulated time for actual delay
 * @mod_ns:		nominal moderation delay, recomputed every epoch
 *
 * Used less frequently, every few interrupts:
 * @epoch_start_ns:	start of current epoch
 * @update_ns:		update_ms from irq_mod_info, converted to ns
 * @stray_irq:		how many stray interrupts (almost never used)
 *
 * Used once per epoch per interrupt source:
 * @descs:		list of	moderated irq_desc on this CPU
 * @enqueue:		how many enqueue on the list
 *
 * Statistics
 * @timer_set:		how many timer_set calls
 */
struct irq_mod_state {
	struct hrtimer		timer;
	bool			moderation_allowed;
	u32			intr_count;
	u32			sleep_ns;
	u32			mod_ns;
	atomic64_t		epoch_start_ns;
	u32			update_ns;
	u32			stray_irq;
	struct list_head	descs;
	u32			enqueue;
	u32			timer_set;
};

DECLARE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

extern struct static_key_false irq_moderation_enabled_key;

bool irq_moderation_do_start(struct irq_desc *desc, struct irq_mod_state *m);

static inline void check_epoch(struct irq_mod_state *m)
{
	const u64 now = ktime_get_ns(), epoch_ns = now - atomic64_read(&m->epoch_start_ns);
	const u32 slack_ns = 5000;

	/* Run approximately every update_ns, a little bit early is ok. */
	if (epoch_ns < m->update_ns - slack_ns)
		return;
	/* Fetch updated parameters. */
	m->update_ns = READ_ONCE(irq_mod_info.update_ms) * NSEC_PER_MSEC;
	m->mod_ns = READ_ONCE(irq_mod_info.delay_us) * NSEC_PER_USEC;
}

/*
 * Use after running the handler, with lock held. If this source should be
 * moderated, disable it, add to the timer list for this CPU and return true.
 * The caller must also exit handle_*_irq() without processing IRQS_PENDING,
 * as that will happen when the moderation timer fires and calls __enable_irq().
 */
static inline bool irq_start_moderation(struct irq_desc *desc)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	if (!static_branch_unlikely(&irq_moderation_enabled_key))
		return false;
	if (!irq_settings_moderation_allowed(desc))
		return false;
	if (!m->moderation_allowed)
		return false;

	m->intr_count++;

	/* Is this a new epoch? ktime_get_ns() is expensive, don't check too often. */
	if ((m->intr_count & 0xf) == 0)
		check_epoch(m);

	return irq_moderation_do_start(desc, m);
}
#else /* CONFIG_IRQ_SW_MODERATION */
static inline bool irq_start_moderation(struct irq_desc *desc) { return false; }
#endif /* !CONFIG_IRQ_SW_MODERATION */

#endif /* _LINUX_IRQ_MODERATION_H */
