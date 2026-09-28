/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _ZL3073X_OUT_H
#define _ZL3073X_OUT_H

#include <linux/bitfield.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "regs.h"

struct zl3073x_dev;

/**
 * struct zl3073x_out - output state
 * @div: output divisor
 * @width: output pulse width
 * @esync_n_period: embedded sync or n-pin period (for n-div formats)
 * @esync_n_width: embedded sync or n-pin pulse width
 * @phase_comp: phase compensation
 * @mode: output mode
 * @gpo_en: GPO override enable for the P-pin and N-pin
 * @gpo_config_p: GPO mode configuration for the P-pin
 * @gpo_config_n: GPO mode configuration for the N-pin
 * @ctrl: output control
 */
struct zl3073x_out {
	struct_group(cfg, /* Config */
		u32	div;
		u32	width;
		u32	esync_n_period;
		u32	esync_n_width;
		s32	phase_comp;
		u8	mode;
		u8	gpo_en;
		u8	gpo_config_p;
		u8	gpo_config_n;
		u8	ctrl;
	);
};

int zl3073x_out_state_fetch(struct zl3073x_dev *zldev, u8 index);
const struct zl3073x_out *zl3073x_out_state_get(struct zl3073x_dev *zldev,
						u8 index);

int zl3073x_out_state_set(struct zl3073x_dev *zldev, u8 index,
			  const struct zl3073x_out *out);

/**
 * zl3073x_out_clock_type_get - get output clock type
 * @out: pointer to out state
 *
 * Return: clock type of given output (ZL_OUTPUT_MODE_CLOCK_TYPE_*)
 */
static inline u8 zl3073x_out_clock_type_get(const struct zl3073x_out *out)
{
	return FIELD_GET(ZL_OUTPUT_MODE_CLOCK_TYPE, out->mode);
}

/**
 * zl3073x_out_clock_type_set - set output clock type
 * @out: pointer to out state
 * @type: clock type (ZL_OUTPUT_MODE_CLOCK_TYPE_*)
 */
static inline void
zl3073x_out_clock_type_set(struct zl3073x_out *out, u8 type)
{
	FIELD_MODIFY(ZL_OUTPUT_MODE_CLOCK_TYPE, &out->mode, type);
}

/**
 * zl3073x_out_signal_format_get - get output signal format
 * @out: pointer to out state
 *
 * Return: signal format of given output
 */
static inline u8 zl3073x_out_signal_format_get(const struct zl3073x_out *out)
{
	return FIELD_GET(ZL_OUTPUT_MODE_SIGNAL_FORMAT, out->mode);
}

/**
 * zl3073x_out_is_diff - check if the given output is differential
 * @out: pointer to out state
 *
 * Return: true if output is differential, false if output is single-ended
 */
static inline bool zl3073x_out_is_diff(const struct zl3073x_out *out)
{
	switch (zl3073x_out_signal_format_get(out)) {
	case ZL_OUTPUT_MODE_SIGNAL_FORMAT_LVDS:
	case ZL_OUTPUT_MODE_SIGNAL_FORMAT_DIFF:
	case ZL_OUTPUT_MODE_SIGNAL_FORMAT_LOWVCM:
		return true;
	default:
		break;
	}

	return false;
}

/**
 * zl3073x_out_is_enabled - check if the given output is enabled
 * @out: pointer to out state
 *
 * Return: true if output is enabled, false if output is disabled
 */
static inline bool zl3073x_out_is_enabled(const struct zl3073x_out *out)
{
	return !!FIELD_GET(ZL_OUTPUT_CTRL_EN, out->ctrl);
}

/**
 * zl3073x_out_is_stopped - check if the given output is stopped
 * @out: pointer to out state
 *
 * Return: true if output clock is stopped, false if it is running
 */
static inline bool zl3073x_out_is_stopped(const struct zl3073x_out *out)
{
	return !!FIELD_GET(ZL_OUTPUT_CTRL_STOP, out->ctrl);
}

/**
 * zl3073x_out_stop - request a clean stop of an output
 * @out: pointer to out state to update
 *
 * Sets the stop and stop_hz bits together, so the output goes high-Z
 * rather than holding a fixed level once stopped. The stop is
 * edge-aligned (the device waits for the proper edge before actually
 * stopping).
 */
static inline void zl3073x_out_stop(struct zl3073x_out *out)
{
	FIELD_MODIFY(ZL_OUTPUT_CTRL_STOP, &out->ctrl, 1);
	FIELD_MODIFY(ZL_OUTPUT_CTRL_STOP_HZ, &out->ctrl, 1);
}

/**
 * zl3073x_out_start - request a clean restart of a stopped output
 * @out: pointer to out state to update
 *
 * Clears the stop and stop_hz bits together. See zl3073x_out_stop().
 */
static inline void zl3073x_out_start(struct zl3073x_out *out)
{
	FIELD_MODIFY(ZL_OUTPUT_CTRL_STOP, &out->ctrl, 0);
	FIELD_MODIFY(ZL_OUTPUT_CTRL_STOP_HZ, &out->ctrl, 0);
}

#define ZL3073X_OUT_PIN_F_CLOCK		0
#define ZL3073X_OUT_PIN_F_GPO_CONST	1
#define ZL3073X_OUT_PIN_F_GPO_STATUS	2
#define ZL3073X_OUT_PIN_F_GPO_IRQ	3
#define ZL3073X_OUT_PIN_F_GPO_UNKNOWN	4

/**
 * zl3073x_out_pin_func_get - get the function of an output pin
 * @out: pointer to out state
 * @id: output pin ID (even for P pin, odd for N pin)
 *
 * Report the current function of the given output pin. If GPO override is
 * disabled the pin acts as a clock, otherwise it acts as a GPO with the
 * mode selected by its GPO config control field.
 *
 * Return: one of the ZL3073X_OUT_PIN_F_* function codes
 */
static inline u8
zl3073x_out_pin_func_get(const struct zl3073x_out *out, u8 id)
{
	u8 gpo_config;
	bool gpo_en;

	if (id & 1) {
		gpo_en = FIELD_GET(ZL_OUTPUT_GPO_EN_OUT_N, out->gpo_en);
		gpo_config = out->gpo_config_n;
	} else {
		gpo_en = FIELD_GET(ZL_OUTPUT_GPO_EN_OUT_P, out->gpo_en);
		gpo_config = out->gpo_config_p;
	}

	if (!gpo_en)
		return ZL3073X_OUT_PIN_F_CLOCK;

	switch (FIELD_GET(ZL_OUTPUT_GPO_CONFIG_CTRL, gpo_config)) {
	case ZL_OUTPUT_GPO_CONFIG_CTRL_OUTPUT:
		return ZL3073X_OUT_PIN_F_GPO_CONST;
	case ZL_OUTPUT_GPO_CONFIG_CTRL_STATUS:
		return ZL3073X_OUT_PIN_F_GPO_STATUS;
	case ZL_OUTPUT_GPO_CONFIG_CTRL_IRQ:
		return ZL3073X_OUT_PIN_F_GPO_IRQ;
	}

	return ZL3073X_OUT_PIN_F_GPO_UNKNOWN;
}

/**
 * zl3073x_out_pin_func_set - set the function of an output pin
 * @out: pointer to out state to update
 * @id: output pin ID (even for P pin, odd for N pin)
 * @func: requested function, one of the ZL3073X_OUT_PIN_F_* codes
 *
 * Configure the given output pin as a clock or as a GPO in the requested
 * mode by updating its GPO enable and GPO config control fields. Unknown
 * function codes are ignored.
 */
static inline void
zl3073x_out_pin_func_set(struct zl3073x_out *out, u8 id, u8 func)
{
	bool gpo_en = true;
	u8 *gpo_config;
	int ctrl = -1;

	switch (func) {
	case ZL3073X_OUT_PIN_F_CLOCK:
		gpo_en = false;
		break;
	case ZL3073X_OUT_PIN_F_GPO_CONST:
		ctrl = ZL_OUTPUT_GPO_CONFIG_CTRL_OUTPUT;
		break;
	case ZL3073X_OUT_PIN_F_GPO_STATUS:
		ctrl = ZL_OUTPUT_GPO_CONFIG_CTRL_STATUS;
		break;
	case ZL3073X_OUT_PIN_F_GPO_IRQ:
		ctrl = ZL_OUTPUT_GPO_CONFIG_CTRL_IRQ;
		break;
	default:
		return;
	}

	if (id & 1) {
		FIELD_MODIFY(ZL_OUTPUT_GPO_EN_OUT_N, &out->gpo_en, gpo_en);
		gpo_config = &out->gpo_config_n;
	} else {
		FIELD_MODIFY(ZL_OUTPUT_GPO_EN_OUT_P, &out->gpo_en, gpo_en);
		gpo_config = &out->gpo_config_p;
	}

	if (ctrl != -1)
		FIELD_MODIFY(ZL_OUTPUT_GPO_CONFIG_CTRL, gpo_config, ctrl);
}

/**
 * zl3073x_out_is_ndiv - check if the given output is in N-div mode
 * @out: pointer to out state
 *
 * Return: true if output is in N-div mode, false otherwise
 */
static inline bool zl3073x_out_is_ndiv(const struct zl3073x_out *out)
{
	switch (zl3073x_out_signal_format_get(out)) {
	case ZL_OUTPUT_MODE_SIGNAL_FORMAT_2_NDIV:
	case ZL_OUTPUT_MODE_SIGNAL_FORMAT_2_NDIV_INV:
		return true;
	default:
		return false;
	}
}

/**
 * zl3073x_out_synth_get - get synth connected to given output
 * @out: pointer to out state
 *
 * Return: index of synth connected to given output.
 */
static inline u8 zl3073x_out_synth_get(const struct zl3073x_out *out)
{
	return FIELD_GET(ZL_OUTPUT_CTRL_SYNTH_SEL, out->ctrl);
}

#endif /* _ZL3073X_OUT_H */
