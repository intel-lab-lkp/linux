/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Intel PPS signal Generator Driver
 *
 * Copyright (C) 2025 Intel Corporation
 */

#ifndef _PPS_GEN_TIO_H_
#define _PPS_GEN_TIO_H_

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/hrtimer.h>
#include <linux/pps_gen_kernel.h>
#include <linux/spinlock_types.h>
#include <linux/time.h>
#include <linux/types.h>

struct device;

#define TIOCTL			0x00
#define TIOCOMPV		0x10
#define TIOEC			0x30

/* Control Register */
#define TIOCTL_EN			BIT(0)
#define TIOCTL_DIR			BIT(1)
#define TIOCTL_EP			GENMASK(3, 2)
#define TIOCTL_EP_RISING_EDGE		FIELD_PREP(TIOCTL_EP, 0)
#define TIOCTL_EP_FALLING_EDGE		FIELD_PREP(TIOCTL_EP, 1)
#define TIOCTL_EP_TOGGLE_EDGE		FIELD_PREP(TIOCTL_EP, 2)

/* Safety time to set hrtimer early */
#define SAFE_TIME_NS			(10 * NSEC_PER_MSEC)

#define MAGIC_CONST			(NSEC_PER_SEC - SAFE_TIME_NS)
#define ART_HW_DELAY_CYCLES		2

struct pps_tio {
	struct pps_gen_source_info gen_info;
	struct pps_gen_device *pps_gen;
	struct hrtimer timer;
	void __iomem *base;
	u32 prev_count;
	spinlock_t lock;
	struct device *dev;
};

#endif /* _PPS_GEN_TIO_H_ */
