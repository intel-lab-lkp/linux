/* SPDX-License-Identifier: GPL-2.0
 *
 * CS40L50 Advanced Haptic Driver with waveform memory,
 * integrated DSP, and closed-loop algorithms
 *
 * Copyright 2023 Cirrus Logic, Inc.
 *
 */

#ifndef __CS40L50_H__
#define __CS40L50_H__

#include <linux/firmware/cirrus/cs_dsp.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/cirrus_haptics.h>
#include <linux/interrupt.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>

/* Power Supply Configuration */
#define CS40L50_BLOCK_ENABLES2			0x201C
#define CS40L50_ERR_RLS				0x2034
#define CS40L50_PWRMGT_CTL			0x2900
#define CS40L50_BST_LPMODE_SEL			0x3810
#define CS40L50_DCM_LOW_POWER		0x1
#define CS40L50_OVERTEMP_WARN		0x4000010

/* Interrupts */
#define CS40L50_IRQ1_INT_1			0xE010
#define CS40L50_IRQ1_INT_2			0xE014
#define CS40L50_IRQ1_INT_8			0xE02C
#define CS40L50_IRQ1_INT_9			0xE030
#define CS40L50_IRQ1_INT_10			0xE034
#define CS40L50_IRQ1_INT_18			0xE054
#define CS40L50_IRQ1_MASK_1			0xE090
#define CS40L50_IRQ1_MASK_2			0xE094
#define CS40L50_IRQ1_MASK_20			0xE0DC
#define CS40L50_IRQ_MASK_2_OVERRIDE	0xFFDF7FFF
#define CS40L50_IRQ_MASK_20_OVERRIDE	0x15C01000
#define CS40L50_AMP_SHORT_MASK		BIT(31)
#define CS40L50_VIRT2_MBOX_MASK		BIT(21)
#define CS40L50_TEMP_ERR_MASK		BIT(31)
#define CS40L50_BST_UVP_MASK		BIT(6)
#define CS40L50_BST_SHORT_MASK		BIT(7)
#define CS40L50_BST_ILIMIT_MASK		BIT(8)
#define CS40L50_UVLO_VDDBATT_MASK	BIT(16)
#define CS40L50_GLOBAL_ERROR_MASK	BIT(15)
#define CS40L50_GLOBAL_ERR_RLS		BIT(11)
#define CS40L50_IRQ(_irq, _name, _hand)		\
	{					\
		.irq = CS40L50_ ## _irq ## _IRQ,\
		.name = _name,			\
		.handler = cs40l50_ ## _hand,	\
	}
#define CS40L50_REG_IRQ(_reg, _irq)					\
	[CS40L50_ ## _irq ## _IRQ] = {					\
		.reg_offset = (CS40L50_ ## _reg) - CS40L50_IRQ1_INT_1,	\
		.mask = CS40L50_ ## _irq ## _MASK			\
	}

/* Mailbox Inbound Commands */
#define CS40L50_RAM_BANK_INDEX_START	0x1000000
#define CS40L50_RTH_INDEX_START		0x1400000
#define CS40L50_RTH_INDEX_END		0x1400001
#define CS40L50_ROM_BANK_INDEX_START	0x1800000
#define CS40L50_ROM_BANK_INDEX_END	0x180001A
#define CS40L50_PREVENT_HIBER		0x2000003
#define CS40L50_ALLOW_HIBER		0x2000004
#define CS40L50_OWT_PUSH		0x3000008
#define CS40L50_STOP_PLAYBACK		0x5000000
#define CS40L50_OWT_DELETE		0xD000000

/* Mailbox Outbound Commands */
#define CS40L50_MBOX_QUEUE_BASE				0x11004
#define CS40L50_MBOX_QUEUE_END				0x1101C
#define CS40L50_DSP_MBOX				0x11020
#define CS40L50_MBOX_QUEUE_WT				0x28042C8
#define CS40L50_MBOX_QUEUE_RD				0x28042CC
#define CS40L50_MBOX_HAPTIC_COMPLETE_MBOX	0x1000000
#define CS40L50_MBOX_HAPTIC_COMPLETE_GPIO	0x1000001
#define CS40L50_MBOX_HAPTIC_COMPLETE_I2S	0x1000002
#define CS40L50_MBOX_HAPTIC_TRIGGER_MBOX	0x1000010
#define CS40L50_MBOX_HAPTIC_TRIGGER_GPIO	0x1000011
#define CS40L50_MBOX_HAPTIC_TRIGGER_I2S		0x1000012
#define CS40L50_MBOX_INIT			0x2000000
#define CS40L50_MBOX_AWAKE			0x2000002
#define CS40L50_MBOX_F0_EST_START		0x7000011
#define CS40L50_MBOX_F0_EST_DONE		0x7000021
#define CS40L50_MBOX_REDC_EST_START		0x7000012
#define CS40L50_MBOX_REDC_EST_DONE		0x7000022
#define CS40L50_MBOX_LE_EST_START		0x7000014
#define CS40L50_MBOX_LE_EST_DONE		0x7000024
#define CS40L50_MBOX_ACK			0xA000000
#define CS40L50_MBOX_PANIC			0xC000000
#define CS40L50_MBOX_WATERMARK			0xD000000
#define CS40L50_MBOX_ERR_EVENT_UNMAPPED		0xC0004B3
#define CS40L50_MBOX_ERR_EVENT_MODIFY		0xC0004B4
#define CS40L50_MBOX_ERR_NULLPTR		0xC0006A5
#define CS40L50_MBOX_ERR_BRAKING		0xC0006A8
#define CS40L50_MBOX_ERR_INVAL_SRC		0xC0006AC
#define CS40L50_MBOX_ERR_ENABLE_RANGE		0xC000836
#define CS40L50_MBOX_ERR_GPIO_UNMAPPED		0xC000837
#define CS40L50_MBOX_ERR_ISR_RANGE		0xC000838
#define CS40L50_MBOX_PERMANENT_SHORT		0xC000C1C
#define CS40L50_MBOX_RUNTIME_SHORT		0xC000C1D

/* DSP */
#define CS40L50_DSP1_XMEM_PACKED_0		0x2000000
#define CS40L50_DSP1_XMEM_UNPACKED32_0		0x2400000
#define CS40L50_SYS_INFO_ID			0x25E0000
#define CS40L50_DSP1_XMEM_UNPACKED24_0		0x2800000
#define CS40L50_RAM_INIT			0x28021DC
#define CS40L50_POWER_ON_SEQ			0x2804320
#define CS40L50_OWT_BASE			0x2805C34
#define CS40L50_NUM_OF_WAVES			0x280CB4C
#define CS40L50_CORE_BASE			0x2B80000
#define CS40L50_CCM_CORE_CONTROL		0x2BC1000
#define CS40L50_DSP1_YMEM_PACKED_0		0x2C00000
#define CS40L50_DSP1_YMEM_UNPACKED32_0		0x3000000
#define CS40L50_DSP1_YMEM_UNPACKED24_0		0x3400000
#define CS40L50_DSP1_PMEM_0			0x3800000
#define CS40L50_DSP1_PMEM_5114			0x3804FE8
#define CS40L50_MEM_RDY_HW		0x2
#define CS40L50_RAM_INIT_FLAG		0x1
#define CS40L50_CLOCK_DISABLE		0x80
#define CS40L50_CLOCK_ENABLE		0x281
#define CS40L50_DSP_POLL_US		1000
#define CS40L50_DSP_TIMEOUT_COUNT	100
#define CS40L50_CP_READY_US		2200
#define CS40L50_PSEQ_SIZE		200
#define CS40L50_AUTOSUSPEND_MS		2000

/* Firmware */
#define CS40L50_FW			"cs40l50.wmfw"
#define CS40L50_WT			"cs40l50.bin"

/* Calibration */
#define CS40L50_REDC_ESTIMATION		0x2802F7C
#define CS40L50_F0_ESTIMATION		0x2802F84
#define CS40L50_F0_STORED		0x2805C00
#define CS40L50_REDC_STORED		0x2805C04
#define CS40L50_RE_EST_STATUS		0x3401B40

/* Open wavetable */
#define CS40L50_OWT_SIZE		0x2805C38
#define CS40L50_OWT_NEXT		0x2805C3C
#define CS40L50_NUM_OF_OWT_WAVES	0x2805C40

/* GPIO */
#define CS40L50_GPIO_BASE		0x2804140

/* Device */
#define CS40L50_DEVID			0x0
#define CS40L50_REVID			0x4
#define CS40L50_DEVID_A		0x40A50
#define CS40L50_REVID_B0	0xB0

enum cs40l50_irq_list {
	CS40L50_GLOBAL_ERROR_IRQ,
	CS40L50_UVLO_VDDBATT_IRQ,
	CS40L50_BST_ILIMIT_IRQ,
	CS40L50_BST_SHORT_IRQ,
	CS40L50_BST_UVP_IRQ,
	CS40L50_TEMP_ERR_IRQ,
	CS40L50_VIRT2_MBOX_IRQ,
	CS40L50_AMP_SHORT_IRQ,
};

struct cs40l50_irq {
	const char *name;
	int irq;
	irqreturn_t (*handler)(int irq, void *data);
};

struct cs40l50_private {
	struct device *dev;
	struct regmap *regmap;
	struct cs_dsp dsp;
	struct mutex lock;
	struct gpio_desc *reset_gpio;
	struct regmap_irq_chip_data *irq_data;
	struct input_dev *input;
	const struct firmware *wmfw;
	struct cs_hap haptics;
	u32 devid;
	u32 revid;
	int irq;
};

int cs40l50_probe(struct cs40l50_private *cs40l50);
int cs40l50_remove(struct cs40l50_private *cs40l50);

extern const struct regmap_config cs40l50_regmap;
extern const struct dev_pm_ops cs40l50_pm_ops;

#endif /* __CS40L50_H__ */
