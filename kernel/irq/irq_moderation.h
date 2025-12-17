/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#ifndef _LINUX_IRQ_MODERATION_H
#define _LINUX_IRQ_MODERATION_H

/* Common data structures for Global Software Interrupt Moderation, GSIM */

#include <linux/hrtimer.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel.h>

#ifdef CONFIG_IRQ_SOFT_MODERATION

/**
 * struct irq_mod_info - global configuration parameters and state
 * @version:		increment on writes to /proc/irq/soft_moderation
 * @delay_us:		maximum delay
 * @update_ms:		how often to update delay (epoch duration)
 */
struct irq_mod_info {
	u32		version;
	u32		delay_us;
	u32		update_ms;
	u32		pad[];
};

extern struct irq_mod_info irq_mod_info;

/**
 * struct irq_mod_state - per-CPU moderation state
 *
 * Used on every interrupt:
 * @timer:		moderation timer
 * @moderation_on:	per-CPU enable, toggled during hotplug/suspend events
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
 * Used once per epoch:
 * @version:		version fetched from irq_mod_info
 *
 * Statistics
 * @timer_set:		how many timer_set calls
 */
struct irq_mod_state {
	/* Used on every interrupt. */
	struct hrtimer		timer;
	bool			moderation_on;
	u32			intr_count;
	u32			sleep_ns;
	u32			mod_ns;

	/* Used less frequently. */
	u64			epoch_start_ns;
	u32			update_ns;
	u32			stray_irq;

	/* Used once per epoch per source. */
	struct list_head	descs;
	u32			enqueue;

	/* Used once per epoch. */
	u32			version;

	/* Statistics */
	u32			timer_set;
};

DECLARE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

extern struct static_key_false irq_moderation_enabled_key;

void irq_moderation_procfs_add(struct irq_desc *desc, umode_t umode);
void irq_moderation_procfs_remove(struct irq_desc *desc);

#else /* CONFIG_IRQ_SOFT_MODERATION */

static inline void irq_moderation_procfs_add(struct irq_desc *desc, umode_t umode) {}
static inline void irq_moderation_procfs_remove(struct irq_desc *desc) {}

#endif /* !CONFIG_IRQ_SOFT_MODERATION */

#endif /* _LINUX_IRQ_MODERATION_H */
