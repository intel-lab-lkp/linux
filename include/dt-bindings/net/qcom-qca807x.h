/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Device Tree constants for the Qualcomm QCA807X PHYs
 */

#ifndef _DT_BINDINGS_QCOM_QCA807X_H
#define _DT_BINDINGS_QCOM_QCA807X_H

/* Full amplitude, full bias current */
#define QCA807X_CONTROL_DAC_FULL_VOLT_BIAS		0
/* Amplitude follow DSP (amplitude is adjusted based on cable length), half bias current */
#define QCA807X_CONTROL_DAC_DSP_VOLT_HALF_BIAS		1
/* Full amplitude, bias current follow DSP (bias current is adjusted based on cable length) */
#define QCA807X_CONTROL_DAC_FULL_VOLT_DSP_BIAS		2
/* Both amplitude and bias current follow DSP */
#define QCA807X_CONTROL_DAC_DSP_VOLT_BIAS		3
/* Full amplitude, half bias current */
#define QCA807X_CONTROL_DAC_FULL_VOLT_HALF_BIAS		4
/* Amplitude follow DSP setting; 1/4 bias current when cable<10m,
 * otherwise half bias current
 */
#define QCA807X_CONTROL_DAC_DSP_VOLT_QUARTER_BIAS	5
/* Full amplitude; same bias current setting with “010” and “011”,
 * but half more bias is reduced when cable <10m
 */
#define QCA807X_CONTROL_DAC_FULL_VOLT_HALF_BIAS_SHORT	6
/* Amplitude follow DSP; same bias current setting with “110”, default value */
#define QCA807X_CONTROL_DAC_DSP_VOLT_HALF_BIAS_SHORT	7

#endif
