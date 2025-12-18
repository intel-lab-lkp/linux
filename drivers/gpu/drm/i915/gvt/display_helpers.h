// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __DISPLAY_HELPERS_H__
#define __DISPLAY_HELPERS_H__

#include "display/intel_gvt_api.h"

#define DISPLAY_MMIO_BASE(display) \
	intel_display_device_mmio_base((display))

#define INTEL_DISPLAY_DEVICE_PIPE_OFFSET(display, idx) \
	intel_display_device_pipe_offset((display), (enum pipe)(idx))

#define INTEL_DISPLAY_DEVICE_TRANS_OFFSET(display, trans) \
	intel_display_device_trans_offset((display), (trans))

#define INTEL_DISPLAY_DEVICE_CURSOR_OFFSET(display, pipe) \
	intel_display_device_cursor_offset((display), (pipe))

#define gvt_for_each_pipe(display, __p) \
	for ((__p) = 0; (__p) < I915_MAX_PIPES; (__p)++) \
		for_each_if(intel_display_device_pipe_valid((display), (enum pipe)(__p)))

#endif /* __DISPLAY_HELPERS_H__ */
