/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_COMMON_H
#define MALI_C720_COMMON_H

#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>

#include "mali-c720-id.h"
#include "mali-c720-register-access.h"
#include "mali-c720-registers.h"
#include "mali-c720-registers-top.h"
#include "mali-c720-registers-ctx-mgr.h"
#include "mali-c720-registers-modules.h"

struct mali_c720_context_regs {
	dma_addr_t dma_handle;
	u8 *base;
	size_t size;

	/* Serializes allocation, initialization and final release. */
	struct mutex allocation_lock;
	refcount_t users;
};

struct mali_c720_context {
	struct mali_c720 *mali_c720;
	struct mali_c720_context_regs regs;
};

struct mali_c720 {
	struct device *dev;
	u8 __iomem *base;
	resource_size_t mmio_size;

	struct mali_c720_registers registers;

	const struct mali_c720_hw_version *hw_version;
};

#endif /* MALI_C720_COMMON_H */
