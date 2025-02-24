/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2020 Samsung Electronics Co., Ltd.
 * Copyright 2020 Google LLC.
 * Copyright 2024 Linaro Ltd.
 */
#ifndef __EXYNOS_ACPM_H__
#define __EXYNOS_ACPM_H__

#include <linux/debugfs.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/types.h>

#include "exynos-acpm-xfer.h"

/**
 * struct acpm_shmem - shared memory configuration information.
 * @reserved:	unused fields.
 * @chans:	offset to array of struct acpm_chan_shmem.
 * @reserved1:	unused fields.
 * @num_chans:	number of channels.
 */
struct acpm_shmem {
	u32 reserved[2];
	u32 chans;
	u32 reserved1[3];
	u32 num_chans;
};

/**
 * struct acpm_queue - exynos acpm queue.
 * @rear:	rear address of the queue.
 * @front:	front address of the queue.
 * @base:	base address of the queue.
 */
struct acpm_queue {
	void __iomem *rear;
	void __iomem *front;
	void __iomem *base;
};

struct device;
struct acpm_chan;

/**
 * struct acpm_info - driver's private data.
 * @shmem:	pointer to the SRAM configuration data.
 * @sram_base:	base address of SRAM.
 * @chans:	pointer to the ACPM channel parameters retrieved from SRAM.
 * @dev:	pointer to the exynos-acpm device.
 * @handle:	instance of acpm_handle to send to clients.
 * @num_chans:	number of channels available for this controller.
 */
struct acpm_info {
	struct acpm_shmem __iomem *shmem;
	void __iomem *sram_base;
	struct acpm_chan *chans;
	struct device *dev;
	struct acpm_handle handle;
	u32 num_chans;
};

#endif /* __EXYNOS_ACPM_H__ */
