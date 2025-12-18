// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __DISPLAY_HELPERS_H__
#define __DISPLAY_HELPERS_H__

#include "display/intel_gvt_api.h"

#ifdef DISPLAY_MMIO_BASE
#undef DISPLAY_MMIO_BASE
#endif
#define DISPLAY_MMIO_BASE(display) \
	intel_display_device_mmio_base((display))

#ifdef INTEL_DISPLAY_DEVICE_PIPE_OFFSET
#undef INTEL_DISPLAY_DEVICE_PIPE_OFFSET
#endif
#define INTEL_DISPLAY_DEVICE_PIPE_OFFSET(display, idx) \
	intel_display_device_pipe_offset((display), (enum pipe)(idx))

#ifdef INTEL_DISPLAY_DEVICE_TRANS_OFFSET
#undef INTEL_DISPLAY_DEVICE_TRANS_OFFSET
#endif
#define INTEL_DISPLAY_DEVICE_TRANS_OFFSET(display, trans) \
	intel_display_device_trans_offset((display), (trans))

#ifdef INTEL_DISPLAY_DEVICE_CURSOR_OFFSET
#undef INTEL_DISPLAY_DEVICE_CURSOR_OFFSET
#endif
#define INTEL_DISPLAY_DEVICE_CURSOR_OFFSET(display, pipe) \
	intel_display_device_cursor_offset((display), (pipe))

#endif /* __DISPLAY_HELPERS_H__ */
