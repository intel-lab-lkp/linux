/* SPDX-License-Identifier: GPL-2.0 */
/*
 * an7581-reg.h  --  Airoha AN7581 audio driver reg definition
 */

#ifndef _AN7581_REG_H_
#define _AN7581_REG_H_

#define AFE_DAC_CON0			0x0

#define ETDM_IN1_CON0			0x5c
#define   RELATCH_SRC			GENMASK(30, 28)
#define   ETDM_CH_NUM			GENMASK(27, 23)
#define   ETDM_WRD_LEN			GENMASK(20, 16)
#define   ETDM_BIT_LEN			GENMASK(15, 11)
#define   ETDM_FMT			GENMASK(8, 6)
#define   ETDM_SYNC			BIT(1)
#define   ETDM_EN			BIT(0)
#define ETDM_IN1_CON1			0x60
#define ETDM_IN1_CON2			0x64
#define   IN_CLK_SRC			GENMASK(12, 10)
#define ETDM_IN1_CON3			0x68
#define   IN_SEL_FS			GENMASK(30, 26)
#define ETDM_IN1_CON4			0x6c
#define   IN_RELATCH			GENMASK(24, 20)
#define   IN_CLK_INV			BIT(18)
#define ETDM_IN1_CON5			0x70
#define ETDM_IN1_CON6			0x74
#define ETDM_OUT1_CON0			0x7c
#define ETDM_OUT1_CON1			0x80
#define ETDM_OUT1_CON2			0x84
#define ETDM_OUT1_CON3			0x88
#define ETDM_OUT1_CON4			0x8c
#define   OUT_RELATCH			GENMASK(28, 24)
#define   OUT_CLK_SRC			GENMASK(8, 6)
#define   OUT_SEL_FS			GENMASK(4, 0)
#define ETDM_OUT1_CON5			0x90
#define   ETDM_CLK_DIV			BIT(12)
#define   OUT_CLK_INV			BIT(9)
#define ETDM_OUT1_CON6			0x94
#define ETDM_OUT1_CON7			0x98

#define AFE_DL1_BASE			0xa8
#define AFE_DL1_END			0xb0
#define AFE_DL1_CUR			0xac

#define AFE_UL1_BASE			0xc4
#define AFE_UL1_END			0xc8
#define AFE_UL1_CUR			0xcc

#define AFE_IRQ0_CON0			0xe4

#define AFE_IRQ_STS			0xf8
#define  AFE_IRQ_STS_PLAY		BIT(1)
#define  AFE_IRQ_STS_RECORD		BIT(0)

#define AFE_IRQ1_CON0			0x100

#define AFE_MAX_REGISTER		AFE_IRQ1_CON0

#endif
