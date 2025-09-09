// SPDX-License-Identifier: GPL-2.0
/*
 * GPU frequency trace points for DRM subsystem
 *
 * This provides GPU frequency tracing support that will be exposed at:
 * /sys/kernel/debug/tracing/events/power/gpu_frequency/
 */

#ifdef CONFIG_DRM_GPU_FREQUENCY_TRACE

#define CREATE_TRACE_POINTS
#include "drm_gpu_frequency_trace.h"

EXPORT_TRACEPOINT_SYMBOL_GPL(gpu_frequency);

#endif /* CONFIG_DRM_GPU_FREQUENCY_TRACE */
