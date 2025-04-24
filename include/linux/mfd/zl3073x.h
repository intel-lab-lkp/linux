/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __LINUX_MFD_ZL3073X_H
#define __LINUX_MFD_ZL3073X_H

#include <linux/mutex.h>

struct device;
struct regmap;

/*
 * Hardware limits for ZL3073x chip family
 */
#define ZL3073X_NUM_INPUTS	10
#define ZL3073X_NUM_OUTPUTS	10
#define ZL3073X_NUM_SYNTHS	5

/**
 * struct zl3073x_dev - zl3073x device
 * @dev: pointer to device
 * @regmap: regmap to access device registers
 */
struct zl3073x_dev {
	struct device		*dev;
	struct regmap		*regmap;
};

/*************************
 * DPLL mailbox operations
 *************************/

/**
 * struct zl3073x_mb_dpll - DPLL channel mailbox
 * @ref_prio: array of input reference priorities
 */
struct zl3073x_mb_dpll {
	u8	ref_prio[ZL3073X_NUM_INPUTS / 2]; /* 4bits per ref */
#define ZL_DPLL_REF_PRIO_REF_P			GENMASK(3, 0)
#define ZL_DPLL_REF_PRIO_REF_N			GENMASK(7, 4)
#define ZL_DPLL_REF_PRIO_MAX			14
#define ZL_DPLL_REF_PRIO_NONE			15
};
#define ZL3073X_MB_DPLL_REF_PRIO(_ref_pair)	BIT(_ref_pair)

int zl3073x_mb_dpll_read(struct zl3073x_dev *zldev, u8 index, u32 fields,
			 struct zl3073x_mb_dpll *mb);
int zl3073x_mb_dpll_write(struct zl3073x_dev *zldev, u8 index, u32 fields,
			  struct zl3073x_mb_dpll *mb);

/***************************
 * Output mailbox operations
 ***************************/

/**
 * struct zl3073x_mb_output - output mailbox
 * @mode: output mode
 * @div: output divisor
 * @width: output width
 * @esync_period: embedded sync period
 * @esync_width: embedded sync width
 * @phase_comp: phase compensation
 */
struct zl3073x_mb_output {
	u8	mode;				/* page 14, offset 0x05 */
#define ZL_OUTPUT_MODE_CLOCK_TYPE		GENMASK(2, 0)
#define ZL_OUTPUT_MODE_CLOCK_TYPE_NORMAL	0
#define ZL_OUTPUT_MODE_CLOCK_TYPE_ESYNC		1
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT		GENMASK(7, 4)
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_DISABLED	0
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_LVDS	1
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_DIFF	2
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_LOWVCM	3
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_2		4
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_1P		5
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_1N		6
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_2_INV	7
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_2_NDIV	12
#define ZL_OUTPUT_MODE_SIGNAL_FORMAT_2_NDIV_INV	15
	u32	div;				/* page 14, offset 0x0c */
	u32	width;				/* page 14, offset 0x10 */
	u32	esync_period;			/* page 14, offset 0x14 */
	u32	esync_width;			/* page 14, offset 0x18 */
	u32	phase_comp;			/* page 14, offset 0x20 */
};
#define ZL3073X_MB_OUTPUT_MODE			BIT(0)
#define ZL3073X_MB_OUTPUT_DIV			BIT(1)
#define ZL3073X_MB_OUTPUT_WIDTH			BIT(2)
#define ZL3073X_MB_OUTPUT_ESYNC_PERIOD		BIT(3)
#define ZL3073X_MB_OUTPUT_ESYNC_WIDTH		BIT(4)
#define ZL3073X_MB_OUTPUT_PHASE_COMP		BIT(5)

int zl3073x_mb_output_read(struct zl3073x_dev *zldev, u8 index, u32 fields,
			   struct zl3073x_mb_output *output);
int zl3073x_mb_output_write(struct zl3073x_dev *zldev, u8 index, u32 fields,
			    const struct zl3073x_mb_output *output);

/************************************
 * Input reference mailbox operations
 ************************************/

/**
 * struct zl3073x_mb_ref - input reference mailbox
 * @freq_base: frequency base
 * @freq_mult: frequency multiplier
 * @ratio_m: FEC ratio numerator
 * @ratio_n: FEC ratio denominator
 * @config: reference configuration
 * @phase_offset_comp: phase offset compensation
 * @sync_ctrl: synchronization control
 * @esync_div: embedded sync divisor
 */
struct zl3073x_mb_ref {
	u16	freq_base;			/* page 10, offset 0x05 */
	u16	freq_mult;			/* page 10, offset 0x07 */
	u16	ratio_m;			/* page 10, offset 0x09 */
	u16	ratio_n;			/* page 10, offset 0x0b */
	u8	config;				/* page 10, offset 0x0d */
#define ZL_REF_CONFIG_ENABLE			BIT(0)
#define ZL_REF_CONFIG_DIFF_EN			BIT(2)
	u64	phase_offset_comp;		/* page 10, offset 0x28 */
	u8	sync_ctrl;			/* page 10, offset 0x2e */
#define ZL_REF_SYNC_CTRL_MODE			GENMASK(2, 0)
#define ZL_REF_SYNC_CTRL_MODE_REFSYNC_PAIR_OFF	0
#define ZL_REF_SYNC_CTRL_MODE_50_50_ESYNC_25_75	2
	u32	esync_div;			/* page 10, offset 0x30 */
#define ZL_REF_ESYNC_DIV_1HZ			0
};
#define ZL3073X_MB_REF_FREQ_BASE		BIT(0)
#define ZL3073X_MB_REF_FREQ_MULT		BIT(1)
#define ZL3073X_MB_REF_RATIO_M			BIT(2)
#define ZL3073X_MB_REF_RATIO_N			BIT(3)
#define ZL3073X_MB_REF_CONFIG			BIT(4)
#define ZL3073X_MB_REF_PHASE_OFFSET_COMP	BIT(5)
#define ZL3073X_MB_REF_SYNC_CTRL		BIT(6)
#define ZL3073X_MB_REF_ESYNC_DIV		BIT(7)

int zl3073x_mb_ref_read(struct zl3073x_dev *zldev, u8 index, u32 fields,
			struct zl3073x_mb_ref *ref);
int zl3073x_mb_ref_write(struct zl3073x_dev *zldev, u8 index, u32 fields,
			 const struct zl3073x_mb_ref *ref);

/**************************
 * Synth mailbox operations
 **************************/

struct zl3073x_mb_synth {
	u16	freq_base;			/* page 13, offset 0x06 */
	u32	freq_mult;			/* page 13, offset 0x08 */
	u16	freq_m;				/* page 13, offset 0x0c */
	u16	freq_n;				/* page 13, offset 0x0e */
};
#define ZL3073X_MB_SYNTH_FREQ_BASE		BIT(0)
#define ZL3073X_MB_SYNTH_FREQ_MULT		BIT(1)
#define ZL3073X_MB_SYNTH_FREQ_M			BIT(2)
#define ZL3073X_MB_SYNTH_FREQ_N			BIT(3)

int zl3073x_mb_synth_read(struct zl3073x_dev *zldev, u8 index, u32 fields,
			   struct zl3073x_mb_synth *mb);
int zl3073x_mb_synth_write(struct zl3073x_dev *zldev, u8 index, u32 fields,
			   struct zl3073x_mb_synth *mb);

#endif /* __LINUX_MFD_ZL3073X_H */
