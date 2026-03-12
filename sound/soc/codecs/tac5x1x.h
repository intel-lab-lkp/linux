/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Header file for tac5x1x codec driver
 *
 * Copyright (C) 2025 Texas Instruments Incorporated - https://www.ti.com
 *
 * Author: Niranjan H Y <niranjan.hy@ti.com>
 */
#ifndef __TAC5X1X_H__

#include <sound/pcm.h>
#include <linux/mfd/tac5x1x/core.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>

#define TAC5X1X_RATES SNDRV_PCM_RATE_8000_192000
#define TAC5X1X_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
			 SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_3LE | \
			 SNDRV_PCM_FMTBIT_S32_LE)

struct tac5x1x_priv {
	struct tac5x1x *tac5x1x;
	struct snd_soc_component *component;
	struct tac5x1x_irqinfo irqinfo;
	u32 ch_enabled;
	/* Flag to prevent duplicate power-up writes.
	 * Protected by ch_lock mutex. Used to ensure only one event
	 * (ADC or DAC) writes PWR_CFG during power-up sequence.
	 */
	struct mutex ch_lock;
	bool pwr_up_done;
	struct delayed_work powerup_work;
};

#endif //__TAC5X1X_H__
