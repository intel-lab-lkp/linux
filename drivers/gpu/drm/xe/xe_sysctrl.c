// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <drm/drm_managed.h>
#include <linux/device.h>
#include <linux/mutex.h>

#include "regs/xe_sysctrl_regs.h"
#include "xe_device.h"
#include "xe_mmio.h"
#include "xe_printk.h"
#include "xe_soc_remapper.h"
#include "xe_sysctrl.h"
#include "xe_sysctrl_mailbox.h"
#include "xe_sysctrl_types.h"

/**
 * DOC: System Controller (sysctrl)
 *
 * The System Controller (sysctrl) is an embedded microcontroller in Intel GPUs
 * responsible for managing various low-level platform functions. Communication
 * between the driver and the System Controller occurs via a mailbox interface,
 * enabling the exchange of commands and responses.
 *
 * This module provides initialization routines and helper functions to interact
 * with the System Controller through the mailbox.
 */

static void xe_sysctrl_fini(void *arg)
{
	struct xe_device *xe = arg;

	xe->soc_remapper.set_sysctrl_region(xe, 0);
}

/**
 * xe_sysctrl_init - Initialize System Controller subsystem
 * @xe: xe device instance
 *
 * Entry point for System Controller initialization, called from xe_device_probe.
 * This function checks platform support and initializes the system controller.
 *
 * Return: 0 on success, error code on failure
 */
int xe_sysctrl_init(struct xe_device *xe)
{
	struct xe_tile *tile = xe_device_get_root_tile(xe);
	struct xe_sysctrl *sc = &xe->sc;
	int ret;

	if (!xe->info.has_sysctrl)
		return 0;

	if (!xe->soc_remapper.set_sysctrl_region)
		return -ENODEV;

	xe->soc_remapper.set_sysctrl_region(xe, SYSCTRL_MAILBOX_INDEX);

	ret = devm_add_action_or_reset(xe->drm.dev, xe_sysctrl_fini, xe);
	if (ret)
		return ret;

	sc->mmio = devm_kzalloc(xe->drm.dev, sizeof(*sc->mmio), GFP_KERNEL);
	if (!sc->mmio)
		return -ENOMEM;

	xe_mmio_init(sc->mmio, tile, tile->mmio.regs, tile->mmio.regs_size);
	sc->mmio->adj_offset = SYSCTRL_BASE;
	sc->mmio->adj_limit = U32_MAX;

	ret = drmm_mutex_init(&xe->drm, &sc->cmd_lock);
	if (ret)
		return ret;

	xe_sysctrl_mailbox_init(sc);

	return 0;
}
