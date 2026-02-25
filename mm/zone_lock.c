// SPDX-License-Identifier: GPL-2.0
#define CREATE_TRACE_POINTS
#include <trace/events/zone_lock.h>

#include <linux/zone_lock.h>

EXPORT_TRACEPOINT_SYMBOL(zone_lock_start_locking);
EXPORT_TRACEPOINT_SYMBOL(zone_lock_acquire_returned);
EXPORT_TRACEPOINT_SYMBOL(zone_lock_released);

#ifdef CONFIG_TRACING

void __zone_lock_do_trace_start_locking(struct zone *zone)
{
	trace_zone_lock_start_locking(zone);
}
EXPORT_SYMBOL(__zone_lock_do_trace_start_locking);

void __zone_lock_do_trace_acquire_returned(struct zone *zone, bool success)
{
	trace_zone_lock_acquire_returned(zone, success);
}
EXPORT_SYMBOL(__zone_lock_do_trace_acquire_returned);

void __zone_lock_do_trace_released(struct zone *zone)
{
	trace_zone_lock_released(zone);
}
EXPORT_SYMBOL(__zone_lock_do_trace_released);

#endif /* CONFIG_TRACING */
