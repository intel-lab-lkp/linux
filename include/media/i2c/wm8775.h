/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
    wm8775.h - definition for wm8775 inputs and outputs

    Copyright (C) 2006 Hans Verkuil (hverkuil@kernel.org)

*/

#ifndef _WM8775_H_
#define _WM8775_H_

/* The WM8775 has 4 inputs and one output. Zero or more inputs
   are multiplexed together to the output. Hence there are
   16 combinations.
   If only one input is active (the normal case) then the
   input values 1, 2, 4 or 8 should be used. */

#define WM8775_AIN1 1
#define WM8775_AIN2 2
#define WM8775_AIN3 4
#define WM8775_AIN4 8

#define WM8775_REG_UNUSED ((u16)-1)

struct wm8775_platform_data {
	u16 reset; /* RESET (R23) */
	u16 zero_cross_timeout; /* Zero cross detect timeout (R7) */
	u16 interface_ctrl; /* Interface control (R11) */
	u16 master_mode; /* Master mode (R12) */
	u16 powerdown; /* Power down (R13) */

	u16 adc_l; /* ADC left (R14) */
	u16 adc_r; /* ADC right (R15) */
	u16 alc_ctrl_1; /* ALC control 1 (R16)*/
	u16 alc_ctrl_2; /* ALC control 2 (R17) */
	u16 alc_ctrl_3; /* ALC control 3 (R18) */
	u16 noise_gate; /* Noise gate (R19) */
	u16 limiter_ctrl; /* Limiter control (R20) */
	u16 adc_mixer; /* ADC mixer control (R21) */

	bool should_set_audio;
};

extern struct wm8775_platform_data wm8775_nova_s_cfg;
extern struct wm8775_platform_data wm8775_standard_cfg;

#endif
