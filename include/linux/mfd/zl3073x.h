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
 * struct zl3073x_input - input invariant info
 * @enabled: input is enabled or disabled
 * @diff: true if input is differential
 */
struct zl3073x_input {
	bool	enabled;
	bool	diff;
};

/**
 * struct zl3073x_output - output invariant info
 * @enabled: output is enabled or disabled
 * @synth: synthesizer the output is connected to
 * @signal_format: output signal format
 */
struct zl3073x_output {
	bool	enabled;
	u8	synth;
	u8	signal_format;
};

/**
 * struct zl3073x_synth - synthesizer invariant info
 * @freq: synthesizer frequency
 * @dpll: ID of DPLL the synthesizer is driven by
 */
struct zl3073x_synth {
	u64	freq;
	u8	dpll;
};

/**
 * struct zl3073x_dev - zl3073x device
 * @dev: pointer to device
 * @regmap: regmap to access device registers
 * @clock_id: clock id of the device
 * @input: array of inputs' invariants
 * @output: array of outputs' invariants
 * @synth: array of synthesizers' invariants
 */
struct zl3073x_dev {
	struct device		*dev;
	struct regmap		*regmap;
	u64			clock_id;

	/* Invariants */
	struct zl3073x_input	input[ZL3073X_NUM_INPUTS];
	struct zl3073x_output	output[ZL3073X_NUM_OUTPUTS];
	struct zl3073x_synth	synth[ZL3073X_NUM_SYNTHS];
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

static inline
bool zl3073x_is_n_pin(u8 index)
{
	/* P-pins indices are even while N-pins are odd */
	return index & 1;
}

static inline
bool zl3073x_is_p_pin(u8 index)
{
	return !zl3073x_is_n_pin(index);
}

/**
 * zl3073x_input_is_diff - check if the given input ref is differential
 * @zldev: pointer to zl3073x device
 * @index: output index
 *
 * Return: true if input is differential, false if input is single-ended
 */
static inline
bool zl3073x_input_is_diff(struct zl3073x_dev *zldev, u8 index)
{
	return zldev->input[index].diff;
}

/**
 * zl3073x_input_is_enabled - check if the given input ref is enabled
 * @zldev: pointer to zl3073x device
 * @index: input index
 *
 * Return: true if input is enabled, false if input is disabled
 */
static inline
bool zl3073x_input_is_enabled(struct zl3073x_dev *zldev, u8 index)
{
	return zldev->input[index].enabled;
}

/**
 * zl3073x_output_is_enabled - check if the given output is enabled
 * @zldev: pointer to zl3073x device
 * @index: output index
 *
 * Return: true if output is enabled, false if output is disabled
 */
static inline
u8 zl3073x_output_is_enabled(struct zl3073x_dev *zldev, u8 index)
{
	return zldev->output[index].enabled;
}

/**
 * zl3073x_output_signal_format_get - get output signal format
 * @zldev: pointer to zl3073x device
 * @index: output index
 *
 * Return: signal format of given output
 */
static inline
u8 zl3073x_output_signal_format_get(struct zl3073x_dev *zldev, u8 index)
{
	return zldev->output[index].signal_format;
}

/**
 * zl3073x_output_synth_get - get synth connected to given output
 * @zldev: pointer to zl3073x device
 * @index: output index
 *
 * Return: index of synth connected to given output.
 */
static inline
u8 zl3073x_output_synth_get(struct zl3073x_dev *zldev, u8 index)
{
	return zldev->output[index].synth;
}

/**
 * zl3073x_synth_dpll_get - get DPLL ID the synth is driven by
 * @zldev: pointer to zl3073x device
 * @index: synth index
 *
 * Return: ID of DPLL the given synthetizer is driven by
 */
static inline
u64 zl3073x_synth_dpll_get(struct zl3073x_dev *zldev, u8 index)
{
	return zldev->synth[index].dpll;
}

/**
 * zl3073x_synth_freq_get - get synth current freq
 * @zldev: pointer to zl3073x device
 * @index: synth index
 *
 * Return: frequency of given synthetizer
 */
static inline
u64 zl3073x_synth_freq_get(struct zl3073x_dev *zldev, u8 index)
{
	return zldev->synth[index].freq;
}

#endif /* __LINUX_MFD_ZL3073X_H */
