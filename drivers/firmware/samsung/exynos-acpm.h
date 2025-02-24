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
 * @reserved2:	unused fields.
 * @log_rear:	rear pointer of APM log queue.
 * @log_front:	front pointer of APM log queue.
 * @log_base:	base address of APM log queue.
 * @log_mlen:	log message length.
 * @log_qlen:	log queue length.
 * @reserved3:	unused fields.
 * @preempt_log_rear:	rear pointer of APM preempt log queue.
 * @preempt_log_front:	front pointer of APM preempt log queue.
 * @preempt_log_base:	base address of APM preempt log queue.
 * @preempt_log_qlen:	preempt log queue length.
 * @reserved4:	unused fields.
 */
struct acpm_shmem {
	u32 reserved[2];
	u32 chans;
	u32 reserved1[3];
	u32 num_chans;
	u32 reserved2[6];
	u32 log_rear;
	u32 log_front;
	u32 log_base;
	u32 log_mlen;
	u32 log_qlen;
	u32 reserved3[24];
	u32 preempt_log_rear;
	u32 preempt_log_front;
	u32 preempt_log_base;
	u32 preempt_log_qlen;
	u32 reserved4[64];
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
struct acpm_log_info;

/**
 * struct acpm_info - driver's private data.
 * @shmem:	pointer to the SRAM configuration data.
 * @sram_base:	base address of SRAM.
 * @log:	pointer to the ACPM logging info.
 * @chans:	pointer to the ACPM channel parameters retrieved from SRAM.
 * @dev:	pointer to the exynos-acpm device.
 * @handle:	instance of acpm_handle to send to clients.
 * @mbox_dbg_chan: mailbox debug channel.
 * @num_chans:	number of channels available for this controller.
 */
struct acpm_info {
	struct acpm_shmem __iomem *shmem;
	void __iomem *sram_base;
	struct acpm_log_info *log;
	struct acpm_chan *chans;
	struct device *dev;
	struct acpm_handle handle;
	unsigned int mbox_dbg_chan;
	u32 num_chans;
};

#ifdef CONFIG_DEBUG_FS
int acpm_debugfs_register(struct acpm_info *acpm);
void acpm_debugfs_remove(void);
#else
static inline int acpm_debugfs_register(struct acpm_info *acpm) {}
static inline void acpm_debugfs_remove(void) {}
#endif

#endif /* __EXYNOS_ACPM_H__ */
