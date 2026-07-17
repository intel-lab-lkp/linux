/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm CLA driver - internal definitions
 *
 * Copyright 2026 Arm Limited.
 */
#ifndef _ARM_CLA_H_
#define _ARM_CLA_H_

#include <linux/types.h>

#include "arm-cla-regs.h"

/* Number of accelerators per CLA */
#define CLA_NUM_ACC		8
#define CLA_NUM_DATA_REGS	8
#define CLA_SRSTATE_LEN		8

/**
 * struct cla_dev - CLA device
 *
 * Immutable state:
 * @cpu:		The CPU this CLA is attached to.
 * @dev:		The platform device.
 */
struct cla_dev {
	unsigned int cpu;
	struct device *dev;
};

#define cla_dbg(dev, fmt, ...) \
	dev_dbg((dev)->dev, "[%u] " fmt, (dev)->cpu, ##__VA_ARGS__)
#define cla_info(dev, fmt, ...) \
	dev_info((dev)->dev, "[%u] " fmt, (dev)->cpu, ##__VA_ARGS__)
#define cla_err(dev, fmt, ...) \
	dev_err((dev)->dev, "[%u] " fmt, (dev)->cpu, ##__VA_ARGS__)

#endif /* _ARM_CLA_H_ */
