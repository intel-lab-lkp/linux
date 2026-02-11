/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ZONE_LOCK_H
#define _LINUX_ZONE_LOCK_H

#include <linux/mmzone.h>
#include <linux/spinlock.h>
#include <linux/tracepoint-defs.h>

DECLARE_TRACEPOINT(zone_lock_start_locking);
DECLARE_TRACEPOINT(zone_lock_acquire_returned);
DECLARE_TRACEPOINT(zone_lock_released);

#ifdef CONFIG_TRACING

void __zone_lock_do_trace_start_locking(struct zone *zone);
void __zone_lock_do_trace_acquire_returned(struct zone *zone, bool success);
void __zone_lock_do_trace_released(struct zone *zone);

static inline void __zone_lock_trace_start_locking(struct zone *zone)
{
	if (tracepoint_enabled(zone_lock_start_locking))
		__zone_lock_do_trace_start_locking(zone);
}

static inline void __zone_lock_trace_acquire_returned(struct zone *zone,
						      bool success)
{
	if (tracepoint_enabled(zone_lock_acquire_returned))
		__zone_lock_do_trace_acquire_returned(zone, success);
}

static inline void __zone_lock_trace_released(struct zone *zone)
{
	if (tracepoint_enabled(zone_lock_released))
		__zone_lock_do_trace_released(zone);
}

#else /* !CONFIG_TRACING */

static inline void __zone_lock_trace_start_locking(struct zone *zone)
{
}

static inline void __zone_lock_trace_acquire_returned(struct zone *zone,
						      bool success)
{
}

static inline void __zone_lock_trace_released(struct zone *zone)
{
}

#endif /* CONFIG_TRACING */

static inline void zone_lock_init(struct zone *zone)
{
	spin_lock_init(&zone->lock);
}

#define zone_lock_irqsave(zone, flags)				\
do {								\
	bool success = true;					\
								\
	__zone_lock_trace_start_locking(zone);			\
	spin_lock_irqsave(&(zone)->lock, flags);		\
	__zone_lock_trace_acquire_returned(zone, success);	\
} while (0)

#define zone_trylock_irqsave(zone, flags)			\
({								\
	bool success;						\
								\
	__zone_lock_trace_start_locking(zone);			\
	success = spin_trylock_irqsave(&(zone)->lock, flags);	\
	__zone_lock_trace_acquire_returned(zone, success);	\
	success;						\
})

static inline void zone_unlock_irqrestore(struct zone *zone, unsigned long flags)
{
	__zone_lock_trace_released(zone);
	spin_unlock_irqrestore(&zone->lock, flags);
}

static inline void zone_lock_irq(struct zone *zone)
{
	bool success = true;

	__zone_lock_trace_start_locking(zone);
	spin_lock_irq(&zone->lock);
	__zone_lock_trace_acquire_returned(zone, success);
}

static inline void zone_unlock_irq(struct zone *zone)
{
	__zone_lock_trace_released(zone);
	spin_unlock_irq(&zone->lock);
}

#endif /* _LINUX_ZONE_LOCK_H */
