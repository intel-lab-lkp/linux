/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Mecha Systems Pvt Ltd
 */

#if !defined(_ETNAVIV_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _ETNAVIV_TRACE_H_

#include <linux/device.h>
#include <linux/tracepoint.h>
#include <linux/types.h>

#include <drm/etnaviv_drm.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM etnaviv
#define TRACE_INCLUDE_FILE etnaviv_trace

/*
 * flags are ETNA_CLOCK_x, see the uAPI header for when the event fires.
 * begin_ns and end_ns are CLOCK_MONOTONIC. The record itself carries the
 * trace clock, which can differ from it by the time spent in suspend.
 */
TRACE_EVENT(etnaviv_gpu_clock_change,
	    TP_PROTO(struct device *dev, u32 flags, u64 begin_ns, u64 end_ns,
		     unsigned long rate_before, unsigned long rate_after),
	    TP_ARGS(dev, flags, begin_ns, end_ns, rate_before, rate_after),

	    TP_STRUCT__entry(
			     __string(dev, dev_name(dev))
			     __field(u32, flags)
			     __field(u64, begin_ns)
			     __field(u64, end_ns)
			     __field(unsigned long, rate_before)
			     __field(unsigned long, rate_after)
			     ),

	    TP_fast_assign(
			   __assign_str(dev);
			   __entry->flags = flags;
			   __entry->begin_ns = begin_ns;
			   __entry->end_ns = end_ns;
			   __entry->rate_before = rate_before;
			   __entry->rate_after = rate_after;
			   ),

	    TP_printk("dev=%s flags=%s begin_ns=%llu end_ns=%llu rate_before=%lu rate_after=%lu",
		      __get_str(dev),
		      __print_flags(__entry->flags, "|",
				    { ETNA_CLOCK_RESUME, "RESUME" },
				    { ETNA_CLOCK_RESET, "RESET" },
				    { ETNA_CLOCK_BUSY, "BUSY" }),
		      __entry->begin_ns, __entry->end_ns,
		      __entry->rate_before, __entry->rate_after)
);

#endif /* _ETNAVIV_TRACE_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#include <trace/define_trace.h>
