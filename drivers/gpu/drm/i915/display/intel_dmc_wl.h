/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2024 Intel Corporation
 */

#ifndef __INTEL_WAKELOCK_H__
#define __INTEL_WAKELOCK_H__

#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/refcount.h>

#include "i915_reg_defs.h"

struct intel_display;

struct intel_dmc_wl {
	/*
	 * There is a bit of a chicken and egg situation where we depend
	 * on runtime info to know that DMC and wakelock are supported
	 * by the hardware, and such information is grabbed via display
	 * MMIO functions, which in turns call intel_dmc_wl_get() and
	 * intel_dmc_wl_put() as part of their regular flow.
	 *
	 * So we need the initialized field to ensure that we turn the
	 * get/put routines into a no-op until we have initialized.
	 */
	bool initialized;
	spinlock_t lock; /* protects enabled, taken, dc_state and refcount */
	bool enabled;
	bool taken;
	refcount_t refcount;
	/*
	 * We are keeping a copy of the enabled DC state because
	 * intel_display.power.domains is protected by a mutex and we do
	 * not want call mutex_lock() in atomic context, where some of
	 * the tracked MMIO operations happen.
	 */
	u32 dc_state;
	struct delayed_work work;
};

void intel_dmc_wl_init(struct intel_display *display);
void intel_dmc_wl_enable(struct intel_display *display, u32 dc_state);
void intel_dmc_wl_disable(struct intel_display *display);
void intel_dmc_wl_get(struct intel_display *display, i915_reg_t reg);
void intel_dmc_wl_put(struct intel_display *display, i915_reg_t reg);
void intel_dmc_wl_get_noreg(struct intel_display *display);
void intel_dmc_wl_put_noreg(struct intel_display *display);

#endif /* __INTEL_WAKELOCK_H__ */
