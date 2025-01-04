/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2024 Intel Corporation
 */

#ifndef __INTEL_CMTG_H__
#define __INTEL_CMTG_H__

struct intel_atomic_state;
struct intel_display;
struct intel_global_state;

int intel_cmtg_init(struct intel_display *display);
void intel_cmtg_readout_hw_state(struct intel_display *display);
int intel_cmtg_force_disabled(struct intel_atomic_state *state);
int intel_cmtg_atomic_check(struct intel_atomic_state *state);
void intel_cmtg_disable(struct intel_atomic_state *state);

#endif /* __INTEL_CMTG_H__ */
