/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#ifndef _LINUX_IRQ_MODERATION_HOOK_H
#define _LINUX_IRQ_MODERATION_HOOK_H

/* Interrupt hooks for Global Software Interrupt Moderation, GSIM */

#include <linux/hrtimer.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel.h>

#include "irq_moderation.h"

#ifdef CONFIG_IRQ_SOFT_MODERATION

static inline void __maybe_new_epoch(struct irq_mod_state *ms)
{
	const u64 now = ktime_get_ns(), epoch_ns = now - ms->epoch_start_ns;
	const u32 slack_ns = 5000;
	u32 version;

	/* Run approximately every update_ns, a little bit early is ok. */
	if (epoch_ns < ms->update_ns - slack_ns)
		return;
	ms->epoch_start_ns = now;
	/* Fetch updated parameters. */
        while ((version = READ_ONCE(irq_mod_info.version)) != ms->version) {
		ms->update_ns = READ_ONCE(irq_mod_info.update_ms) * NSEC_PER_MSEC;
		ms->mod_ns = READ_ONCE(irq_mod_info.delay_us) * NSEC_PER_USEC;
		ms->version = version;
        }
}

static inline bool irq_moderation_needed(struct irq_desc *desc, struct irq_mod_state *ms)
{
	if (!hrtimer_is_queued(&ms->timer)) {
		const u32 min_delay_ns = 10000;
		const u64 slack_ns = 2000;

		/* Accumulate sleep time, no moderation if too small. */
		ms->sleep_ns += ms->mod_ns;
		if (ms->sleep_ns < min_delay_ns)
			return false;
		/* We need moderation, start the timer. */
		ms->timer_set++;
		hrtimer_start_range_ns(&ms->timer, ns_to_ktime(ms->sleep_ns),
				       slack_ns, HRTIMER_MODE_REL_PINNED_HARD);
	}

	/*
	 * Add to the timer list and __disable_irq() to prevent serving subsequent
	 * interrupts.
	 */
	if (!list_empty(&desc->mod.ms_node)) {
		/* Very unlikely, stray interrupt while moderated. */
		ms->stray_irq++;
	} else {
		ms->enqueue++;
		list_add(&desc->mod.ms_node, &ms->descs);
		__disable_irq(desc);
	}
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	return true;
}

/*
 * Use after running the handler, with lock held. If this source should be
 * moderated, disable it, add to the timer list for this CPU and return true.
 */
static inline bool irq_moderation_hook(struct irq_desc *desc)
{
	struct irq_mod_state *ms = this_cpu_ptr(&irq_mod_state);

	if (!static_branch_unlikely(&irq_moderation_enabled_key))
		return false;
	if (!desc->mod.enable)
		return false;
	if (!ms->moderation_on)
		return false;

	ms->intr_count++;

	/* Is this a new epoch? ktime_get_ns() is expensive, don't check too often. */
	if ((ms->intr_count & 0xf) == 0)
		__maybe_new_epoch(ms);

	return irq_moderation_needed(desc, ms);
}

/* On entry of desc->irq_handler() tell handler to skip moderated interrupts. */
static inline bool irq_moderation_skip_moderated(struct irq_desc *desc)
{
	return !list_empty(&desc->mod.ms_node);
}

#else /* CONFIG_IRQ_SOFT_MODERATION */

static inline bool irq_moderation_hook(struct irq_desc *desc) { return false; }
static inline bool irq_moderation_skip_moderated(struct irq_desc *desc) { return false; }

#endif /* !CONFIG_IRQ_SOFT_MODERATION */

#endif /* _LINUX_IRQ_MODERATION_HOOK_H */
