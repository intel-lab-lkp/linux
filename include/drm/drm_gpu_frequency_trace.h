/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(_GPU_FREQUENCY_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _GPU_FREQUENCY_TRACE_H

#include <linux/tracepoint.h>

#ifdef CONFIG_DRM_GPU_FREQUENCY_TRACE

#undef TRACE_SYSTEM
#define TRACE_SYSTEM power
#define TRACE_INCLUDE_FILE drm_gpu_frequency_trace

/*
 * Tracepoint for GPU frequency changes
 * This tracepoint is exposed at /sys/kernel/debug/tracing/events/power/gpu_frequency
 *
 * location: /sys/kernel/debug/tracing/events/power/gpu_frequency
 * format: {unsigned int state, unsigned int gpu_id}
 * where state holds the frequency(in KHz) and the gpu_id holds the GPU clock domain.
 */
TRACE_EVENT(gpu_frequency,
	    TP_PROTO(unsigned int state, unsigned int gpu_id),
	    TP_ARGS(state, gpu_id),
	    TP_STRUCT__entry(
		    __field(unsigned int, state)
		    __field(unsigned int, gpu_id)
		    ),
	    TP_fast_assign(
		    __entry->state = state;
		    __entry->gpu_id = gpu_id;
		    ),
	    TP_printk("state=%u gpu_id=%u", __entry->state, __entry->gpu_id)
);

#else /* !CONFIG_DRM_GPU_FREQUENCY_TRACE */

static inline void trace_gpu_frequency(unsigned int state, unsigned int gpu_id) { }

#endif /* CONFIG_DRM_GPU_FREQUENCY_TRACE */

#endif /* _GPU_FREQUENCY_TRACE_H */

#ifdef CONFIG_DRM_GPU_FREQUENCY_TRACE
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../include/drm
#include <trace/define_trace.h>
#endif
