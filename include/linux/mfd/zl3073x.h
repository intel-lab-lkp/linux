/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __LINUX_MFD_ZL3073X_H
#define __LINUX_MFD_ZL3073X_H

#include <linux/mutex.h>
#include <linux/types.h>

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
 * @multiop_lock: to serialize multiple register operations
 * @clock_id: clock id of the device
 * @input: array of inputs' invariants
 * @output: array of outputs' invariants
 * @synth: array of synthesizers' invariants
 */
struct zl3073x_dev {
	struct device		*dev;
	struct regmap		*regmap;
	struct mutex		multiop_lock;
	u64			clock_id;

	/* Invariants */
	struct zl3073x_input	input[ZL3073X_NUM_INPUTS];
	struct zl3073x_output	output[ZL3073X_NUM_OUTPUTS];
	struct zl3073x_synth	synth[ZL3073X_NUM_SYNTHS];
};

/**********************
 * Registers operations
 **********************/

int zl3073x_poll_zero_u8(struct zl3073x_dev *zldev, unsigned int reg, u8 mask);
int zl3073x_read_u8(struct zl3073x_dev *zldev, unsigned int reg, u8 *val);
int zl3073x_read_u16(struct zl3073x_dev *zldev, unsigned int reg, u16 *val);
int zl3073x_read_u32(struct zl3073x_dev *zldev, unsigned int reg, u32 *val);
int zl3073x_read_u48(struct zl3073x_dev *zldev, unsigned int reg, u64 *val);
int zl3073x_write_u8(struct zl3073x_dev *zldev, unsigned int reg, u8 val);
int zl3073x_write_u16(struct zl3073x_dev *zldev, unsigned int reg, u16 val);
int zl3073x_write_u32(struct zl3073x_dev *zldev, unsigned int reg, u32 val);
int zl3073x_write_u48(struct zl3073x_dev *zldev, unsigned int reg, u64 val);

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
