/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ZL3073X_REGS_H
#define __ZL3073X_REGS_H

#include <linux/mfd/zl3073x-regs.h>

/**************************
 * Register Page 0, General
 **************************/

#define ZL_REG_ID				ZL_REG(0, 0x01, 2)
#define ZL_REG_REVISION				ZL_REG(0, 0x03, 2)
#define ZL_REG_FW_VER				ZL_REG(0, 0x05, 2)
#define ZL_REG_CUSTOM_CONFIG_VER		ZL_REG(0, 0x07, 4)

/***********************************
 * Register Page 9, Synth and Output
 ***********************************/

#define ZL_REG_SYNTH_CTRL(_idx)						\
	ZL_REG_IDX(_idx, 9, 0x00, 1, ZL3073X_NUM_SYNTHS, 1)
#define ZL_SYNTH_CTRL_EN			BIT(0)
#define ZL_SYNTH_CTRL_DPLL_SEL			GENMASK(6, 4)

#define ZL_REG_OUTPUT_CTRL(_idx)					\
	ZL_REG_IDX(_idx, 9, 0x28, 1, ZL3073X_NUM_OUTPUTS, 1)
#define ZL_OUTPUT_CTRL_EN			BIT(0)
#define ZL_OUTPUT_CTRL_SYNTH_SEL		GENMASK(6, 4)

/*******************************
 * Register Page 10, Ref Mailbox
 *******************************/

#define ZL_REG_REF_CONFIG			ZL_REG(10, 0x0d, 1)
#define ZL_REF_CONFIG_ENABLE			BIT(0)
#define ZL_REF_CONFIG_DIFF_EN			BIT(2)

/*********************************
 * Register Page 13, Synth Mailbox
 *********************************/

#define ZL_REG_SYNTH_MB_MASK			ZL_REG(13, 0x02, 2)

#define ZL_REG_SYNTH_MB_SEM			ZL_REG(13, 0x04, 1)
#define ZL_SYNTH_MB_SEM_WR			BIT(0)
#define ZL_SYNTH_MB_SEM_RD			BIT(1)

#define ZL_REG_SYNTH_FREQ_BASE			ZL_REG(13, 0x06, 2)
#define ZL_REG_SYNTH_FREQ_MULT			ZL_REG(13, 0x08, 4)
#define ZL_REG_SYNTH_FREQ_M			ZL_REG(13, 0x0c, 2)
#define ZL_REG_SYNTH_FREQ_N			ZL_REG(13, 0x0e, 2)

#endif /* __ZL3073X_REGS_H */
