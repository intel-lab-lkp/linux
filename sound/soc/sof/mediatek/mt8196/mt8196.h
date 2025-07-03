/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * Copyright(c) 2025 MediaTek Inc. All rights reserved.
 * Author: Hailong Fan <hailong.fan@mediatek.com>
 */

#ifndef __MT8196_H
#define __MT8196_H

struct mtk_adsp_chip_info;
struct snd_sof_dev;

#define DSP_REG_BAR			4
#define DSP_SECREG_BAR			5
#define DSP_BUSREG_BAR			6
#define CHAN_MIN			1
#define CHAN_MAX			2

/*****************************************************************************
 *                  R E G I S T E R       TABLE
 *****************************************************************************/
/* dsp cfg */
#define ADSP_CFGREG_SW_RSTN		0x0000
#define SW_DBG_RSTN_C0			BIT(0)
#define SW_RSTN_C0			BIT(4)
#define ADSP_IO_CONFIG			0x000C
#define ADSP_CLK_SEL			BIT(31)
#define ADSP_HIFI_RUNSTALL		0x0108
#define TRACEMEMREADY			BIT(15)
#define RUNSTALL			BIT(12)
#define ADSP_IRQ_MASK			0x0030
#define ADSP_DVFSRC_REQ			0x0040
#define ADSP_DDREN_REQ_0		0x0044
#define ADSP_SEMAPHORE			0x0064
#define ADSP_WDT_CON_C0			0x007C
#define ADSP_MBOX_IRQ_EN		0x009C
#define DSP_MBOX0_IRQ_EN		BIT(0)
#define DSP_MBOX1_IRQ_EN		BIT(1)
#define DSP_MBOX2_IRQ_EN		BIT(2)
#define DSP_MBOX3_IRQ_EN		BIT(3)
#define DSP_MBOX4_IRQ_EN		BIT(4)
#define DSP_PDEBUGPC			0x013C
#define DSP_PDEBUGDATA			0x0140
#define DSP_PDEBUGINST			0x0144
#define DSP_PDEBUGLS0STAT		0x0148
#define DSP_PDEBUGSTATUS		0x014C
#define DSP_PFAULTINFO			0x0150
#define ADSP_CK_EN			0x1000
#define DMA1_EN				BIT(12)
#define DMA_AXI_EN			BIT(13)
#define UART_BT_EN			BIT(14)
#define DMA_EN				BIT(4)
#define UART_EN				BIT(5)
#define ADSP_UART_CTRL			0x1010
#define UART_BCLK_CG			BIT(0)
#define UART_RSTN			BIT(3)

/* dsp sec */
#define ADSP_PRID			0x0
#define ADSP_ALTVEC_C0			0x04
#define ADSP_ALTVECSEL			0x0C
#define MT8196_ADSP_ALTVECSEL_C0	1

#define ADSP_ALTVECSEL_C0		MT8196_ADSP_ALTVECSEL_C0

/* dsp bus */
#define AUDIO_BUS_CFG_RSV_10		0x30
#define ADSP_SRAM_POOL_CON		0x190
#define ADSP_SRAM_POOL_ACK		0x1A0
#define ADSP_SRAM_CHANNEL_NUM		6
#define DSP_SRAM_POOL_PD_MASK		0xF00F /* [0:3] and [12:15] */
#define DSP_C0_EMI_MAP_ADDR		0xA00  /* ADSP Core0 To EMI Address Remap */
#define DSP_C0_DMAEMI_MAP_ADDR		0xA08  /* DMA0 To EMI Address Remap */
#define AUDIO_BUS_CFG_BUS_REMAP_CTRL	0x016C
#define AUDIO_BUS_DSP2EMI_REMAP0	0x0A00
#define AUDIO_BUS_DSP2EMI_REMAP1	0x0A04
#define AUDIO_BUS_DMA2EMI_REMAP0	0x0A08
#define AUDIO_BUS_DMA2EMI_REMAP1	0x0A0C

/* DSP memories */
#define MBOX_OFFSET			0x500000 /* DRAM */
#define MBOX_SIZE			0x1000   /* consistent with which in memory.h of sof fw */
#define DSP_DRAM_SIZE			0x900000 /* 9M */

/*remap dram between AP and DSP view, 4KB aligned*/
#define SRAM_PHYS_BASE_FROM_DSP_VIEW	0x4e100000 /* MT8196 DSP view */
#define DRAM_PHYS_BASE_FROM_DSP_VIEW	0x90000000 /* MT8196 DSP view */
#define DRAM_REMAP_SHIFT		12
#define DRAM_REMAP_MASK			0xFFF

/* remap */
#define ADSP_REMAP_REGION
#define ADSP_EMI_REMAP_SET_TOTAL	(4)
#define EMI_REMAP_REGION		(AUDIO_BUS_CFG_RSV_10)
#define EMI_REMAP_REPLACE		(AUDIO_BUS_DSP2EMI_REMAP0)
#define EMI_REMAP_EN			(1U)
#define ADSP_REMAP_REGION_TO(start)	(((start) >> 25) << 9)
#define UPPER_16_SHIFT			16
#define LOWER_16_MASK			0xFFFF
#define UPPER_16_MASK			0xFFFF0000
#define REMAP_ALIGN_SIZE		0x10000

#define SIZE_SHARED_DRAM_DL		0x40000 /*Shared buffer for Downlink*/
#define SIZE_SHARED_DRAM_UL		0x40000 /*Shared buffer for Uplink*/
#define TOTAL_SIZE_SHARED_DRAM_FROM_TAIL	(SIZE_SHARED_DRAM_DL + SIZE_SHARED_DRAM_UL)

void mt8196_sof_hifixdsp_boot_sequence(struct snd_sof_dev *sdev, u32 boot_addr);
void mt8196_sof_hifixdsp_shutdown(struct snd_sof_dev *sdev);

static inline u32 mix_begin_and_end_addr(u32 begin, u32 end)
{
	return ((begin >> UPPER_16_SHIFT) & LOWER_16_MASK) | (end & UPPER_16_MASK);
}

static inline u32 remap_roundup(u32 val, u32 align)
{
	return (val + (align - 1)) & ~(align - 1);
}

static inline u32 adsp_remap_region_from(u32 start, u32 size)
{
	return mix_begin_and_end_addr(start, remap_roundup(start + size, REMAP_ALIGN_SIZE));
}
#endif
