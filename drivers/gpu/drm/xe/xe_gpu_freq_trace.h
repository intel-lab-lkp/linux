/* SPDX-License-Identifier: GPL-2.0 */
/*
 * GPU frequency trace wrapper for xe_pmu.c
 * This header provides access to the gpu_frequency tracepoint
 */
#ifndef _XE_GPU_FREQ_TRACE_H_
#define _XE_GPU_FREQ_TRACE_H_

#include "../drm_gpu_frequency_trace.h"

/* Convert MHz to KHz for tracepoint */
#define MHZ_TO_KHZ(freq_mhz)	((freq_mhz) * 1000)

#endif /* _XE_GPU_FREQ_TRACE_H_ */
