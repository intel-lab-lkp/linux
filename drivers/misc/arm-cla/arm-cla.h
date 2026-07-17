/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm CLA driver - internal definitions
 *
 * Copyright 2026 Arm Limited.
 */
#ifndef _ARM_CLA_H_
#define _ARM_CLA_H_

#include <linux/device.h>
#include <linux/io.h>
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
 * @regs:		Registers accessed by the kernel.
 * @dev:		The platform device.
 */
struct cla_dev {
	unsigned int cpu;
	void __iomem *regs;
	struct device *dev;
};

#define cla_dbg(dev, fmt, ...) \
	dev_dbg((dev)->dev, "[%u] " fmt, (dev)->cpu, ##__VA_ARGS__)
#define cla_info(dev, fmt, ...) \
	dev_info((dev)->dev, "[%u] " fmt, (dev)->cpu, ##__VA_ARGS__)
#define cla_err(dev, fmt, ...) \
	dev_err((dev)->dev, "[%u] " fmt, (dev)->cpu, ##__VA_ARGS__)

static inline u64 cla_reg_read(struct cla_dev *dev, off_t reg)
{
	return readq_relaxed(dev->regs + reg);
}

static inline void cla_reg_write(struct cla_dev *dev, off_t reg, u64 val)
{
	return writeq_relaxed(val, dev->regs + reg);
}

int cla_op_wait_lresp(struct cla_dev *dev, u64 *lresp);
int cla_op_reset(struct cla_dev *dev, unsigned int accid);
int cla_op_regread(struct cla_dev *dev, unsigned int accid, unsigned int regidx,
		   size_t nregs, u64 *regs);
int cla_op_regwrite(struct cla_dev *dev, unsigned int accid,
		    unsigned int regidx, size_t nregs, u64 *regs);

#endif /* _ARM_CLA_H_ */
