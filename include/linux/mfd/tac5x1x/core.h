/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mfd/tac5x1x/core.h -- TAC5x1x Core Interface
 *
 * Copyright 2025 Texas Instruments Incorporated
 *
 * Author: Niranjan H Y <niranjan.hy@ti.com>
 */
#ifndef __MFD_TAC5X1X_CORE_H__
#define __MFD_TAC5X1X_CORE_H__

#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

enum tac5x1x_type {
	TAA5212 = 0,
	TAA5412,
	TAC5111,
	TAC5112,
	TAC5211,
	TAC5212,
	TAC5301,
	TAC5311,
	TAC5312,
	TAC5411,
	TAC5412,
	TAD5112,
	TAD5212,
};

struct tac5x1x_input_diag_config {
	s32 in_ch_en;
	s32 out_ch_en;
	s32 incl_se_inm;
	s32 incl_ac_coup;
};

struct tac5x1x_irqinfo {
	s32 irq_gpio;
	s32 irq;
	bool irq_enable;
	u32 *latch_regs;
	u8 *latch_data;
};

#define TAC5X1X_NUM_SUPPLIES 2

struct tac5x1x {
	enum tac5x1x_type codec_type;
	s32 vref_vg;
	s32 micbias_vg;
	s32 uad_en;
	s32 vad_en;
	s32 uag_en;
	s32 micbias_thr[2];
	s32 gpa_threshold[2];
	s32 adc_impedance[2];
	s32 out2x_vcom_cfg;
	bool pdm_enabled;
	bool regulators_enabled;
	struct tac5x1x_input_diag_config input_diag_config;
	struct regulator_bulk_data supplies[TAC5X1X_NUM_SUPPLIES];
	struct device *dev;
	struct regmap *regmap;
};

#endif
