/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_WRITEBACK_H__
#define __INTEL_WRITEBACK_H__

#include <linux/types.h>

#include "intel_display_types.h"

struct intel_atomic_state;
struct intel_display;
struct intel_writeback_connector;

int intel_writeback_init(struct intel_display *display);
void intel_writeback_atomic_commit(struct intel_atomic_state *state);
void intel_writeback_isr_handler(struct intel_display *display);
bool intel_writeback_transcoder_is_wd(enum transcoder transcoder);

#endif /* __INTEL_WRITEBACK_H__ */

