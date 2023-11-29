// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
/*
 * Realtek RTD1319 interrupt controller driver
 *
 * Copyright (c) 2023 Realtek Semiconductor Corporation
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "irq-realtek-intc-common.h"

#define ISO_NORMAL_MASK      0xffffcffe
#define ISO_RTC_MASK         0x00003001
#define MISC_NMI_WDT_MASK    0x00000004
#define MISC_NORMAL_MASK     0xffffc0d2
#define MISC_UART1_MASK      0x00000028
#define MISC_UART2_MASK      0x00002100

#define ISO_ISR_EN_OFFSET    0x40
#define ISO_ISR_OFFSET       0
#define ISO_ISR_UMSK_OFFSET  0x4
#define MISC_ISR_EN_OFFSET   0x80
#define MISC_ISR_OFFSET      0xc
#define MISC_ISR_UMSK_OFFSET 0x8

enum rtd1319_iso_isr_bits {
	RTD1319_ISO_ISR_TC3_SHIFT	 = 1,
	RTD1319_ISO_ISR_UR0_SHIFT	 = 2,
	RTD1319_ISO_ISR_LSADC0_SHIFT	 = 3,
	RTD1319_ISO_ISR_IRDA_SHIFT	 = 5,
	RTD1319_ISO_ISR_SPI1_SHIFT	 = 6,
	RTD1319_ISO_ISR_WDOG_NMI_SHIFT	 = 7,
	RTD1319_ISO_ISR_I2C0_SHIFT	 = 8,
	RTD1319_ISO_ISR_TC4_SHIFT	 = 9,
	RTD1319_ISO_ISR_TC7_SHIFT	 = 10,
	RTD1319_ISO_ISR_I2C1_SHIFT	 = 11,
	RTD1319_ISO_ISR_RTC_HSEC_SHIFT	 = 12,
	RTD1319_ISO_ISR_RTC_ALARM_SHIFT	 = 13,
	RTD1319_ISO_ISR_GPIOA_SHIFT	 = 19,
	RTD1319_ISO_ISR_GPIODA_SHIFT	 = 20,
	RTD1319_ISO_ISR_ISO_MISC_SHIFT	 = 21,
	RTD1319_ISO_ISR_CBUS_SHIFT	 = 22,
	RTD1319_ISO_ISR_ETN_SHIFT	 = 23,
	RTD1319_ISO_ISR_USB_HOST_SHIFT	 = 24,
	RTD1319_ISO_ISR_USB_U3_DRD_SHIFT = 25,
	RTD1319_ISO_ISR_USB_U2_DRD_SHIFT = 26,
	RTD1319_ISO_ISR_PORB_HV_SHIFT	 = 28,
	RTD1319_ISO_ISR_PORB_DV_SHIFT	 = 29,
	RTD1319_ISO_ISR_PORB_AV_SHIFT	 = 30,
	RTD1319_ISO_ISR_I2C1_REQ_SHIFT	 = 31,
};

static const u32 rtd1319_iso_isr_to_scpu_int_en_mask[32] = {
	[RTD1319_ISO_ISR_SPI1_SHIFT]	  = BIT(1),
	[RTD1319_ISO_ISR_UR0_SHIFT]	  = BIT(2),
	[RTD1319_ISO_ISR_LSADC0_SHIFT]	  = BIT(3),
	[RTD1319_ISO_ISR_IRDA_SHIFT]	  = BIT(5),
	[RTD1319_ISO_ISR_I2C0_SHIFT]	  = BIT(8),
	[RTD1319_ISO_ISR_I2C1_SHIFT]	  = BIT(11),
	[RTD1319_ISO_ISR_RTC_HSEC_SHIFT]  = BIT(12),
	[RTD1319_ISO_ISR_RTC_ALARM_SHIFT] = BIT(13),
	[RTD1319_ISO_ISR_GPIOA_SHIFT]	  = BIT(19),
	[RTD1319_ISO_ISR_GPIODA_SHIFT]	  = BIT(20),
	[RTD1319_ISO_ISR_PORB_HV_SHIFT]	  = BIT(28),
	[RTD1319_ISO_ISR_PORB_DV_SHIFT]	  = BIT(29),
	[RTD1319_ISO_ISR_PORB_AV_SHIFT]	  = BIT(30),
	[RTD1319_ISO_ISR_I2C1_REQ_SHIFT]  = BIT(31),
};

enum rtd1319_misc_isr_bits {
	RTD1319_ISR_WDOG_NMI_SHIFT = 2,
	RTD1319_ISR_UR1_SHIFT	   = 3,
	RTD1319_ISR_TC5_SHIFT	   = 4,
	RTD1319_ISR_UR1_TO_SHIFT   = 5,
	RTD1319_ISR_TC0_SHIFT	   = 6,
	RTD1319_ISR_TC1_SHIFT	   = 7,
	RTD1319_ISR_UR2_SHIFT	   = 8,
	RTD1319_ISR_RTC_HSEC_SHIFT = 9,
	RTD1319_ISR_RTC_MIN_SHIFT  = 10,
	RTD1319_ISR_RTC_HOUR_SHIFT = 11,
	RTD1319_ISR_RTC_DATE_SHIFT = 12,
	RTD1319_ISR_UR2_TO_SHIFT   = 13,
	RTD1319_ISR_I2C5_SHIFT	   = 14,
	RTD1319_ISR_I2C3_SHIFT	   = 23,
	RTD1319_ISR_SC0_SHIFT	   = 24,
	RTD1319_ISR_SC1_SHIFT	   = 25,
	RTD1319_ISR_SPI_SHIFT	   = 27,
	RTD1319_ISR_FAN_SHIFT	   = 29,
};

static const u32 rtd1319_misc_isr_to_scpu_int_en_mask[32] = {
	[RTD1319_ISR_UR1_SHIFT]	     = BIT(3),
	[RTD1319_ISR_UR1_TO_SHIFT]   = BIT(5),
	[RTD1319_ISR_UR2_TO_SHIFT]   = BIT(6),
	[RTD1319_ISR_UR2_SHIFT]	     = BIT(7),
	[RTD1319_ISR_RTC_MIN_SHIFT]  = BIT(10),
	[RTD1319_ISR_RTC_HOUR_SHIFT] = BIT(11),
	[RTD1319_ISR_RTC_DATE_SHIFT] = BIT(12),
	[RTD1319_ISR_I2C5_SHIFT]     = BIT(14),
	[RTD1319_ISR_SC0_SHIFT]	     = BIT(24),
	[RTD1319_ISR_SC1_SHIFT]	     = BIT(25),
	[RTD1319_ISR_SPI_SHIFT]	     = BIT(27),
	[RTD1319_ISR_I2C3_SHIFT]     = BIT(28),
	[RTD1319_ISR_FAN_SHIFT]	     = BIT(29),
	[RTD1319_ISR_WDOG_NMI_SHIFT] = IRQ_ALWAYS_ENABLED,
};

static struct realtek_intc_subset_cfg rtd1319_intc_iso_cfgs[] = {
	{ ISO_NORMAL_MASK, },
	{ ISO_RTC_MASK, },
};

static const struct realtek_intc_info rtd1319_intc_iso_info = {
	.isr_offset		 = ISO_ISR_OFFSET,
	.umsk_isr_offset	 = ISO_ISR_UMSK_OFFSET,
	.scpu_int_en_offset	 = ISO_ISR_EN_OFFSET,
	.isr_to_scpu_int_en_mask = rtd1319_iso_isr_to_scpu_int_en_mask,
	.cfg			 = rtd1319_intc_iso_cfgs,
	.cfg_num		 = ARRAY_SIZE(rtd1319_intc_iso_cfgs),
};

static struct realtek_intc_subset_cfg rtd1319_intc_misc_cfgs[] = {
	{ MISC_NORMAL_MASK, },
	{ MISC_NMI_WDT_MASK, },
	{ MISC_UART1_MASK, },
	{ MISC_UART2_MASK, },
};

static const struct realtek_intc_info rtd1319_intc_misc_info = {
	.isr_offset		 = MISC_ISR_OFFSET,
	.umsk_isr_offset	 = MISC_ISR_UMSK_OFFSET,
	.scpu_int_en_offset	 = MISC_ISR_EN_OFFSET,
	.isr_to_scpu_int_en_mask = rtd1319_misc_isr_to_scpu_int_en_mask,
	.cfg			 = rtd1319_intc_misc_cfgs,
	.cfg_num		 = ARRAY_SIZE(rtd1319_intc_misc_cfgs),
};

static const struct of_device_id realtek_intc_rtd1319_dt_matches[] = {
	{
		.compatible = "realtek,rtd1319-intc-iso",
		.data = &rtd1319_intc_iso_info,
	}, {
		.compatible = "realtek,rtd1319-intc-misc",
		.data = &rtd1319_intc_misc_info,
	},
	{ /* sentinel */ }
};

static int realtek_intc_rtd1319_suspend(struct device *dev)
{
	struct realtek_intc_data *data = dev_get_drvdata(dev);
	const struct realtek_intc_info *info = data->info;

	data->saved_en = readl(data->base + info->scpu_int_en_offset);

	writel(DISABLE_INTC, data->base + info->scpu_int_en_offset);
	writel(CLEAN_INTC_STATUS, data->base + info->umsk_isr_offset);
	writel(CLEAN_INTC_STATUS, data->base + info->isr_offset);

	return 0;
}

static int realtek_intc_rtd1319_resume(struct device *dev)
{
	struct realtek_intc_data *data = dev_get_drvdata(dev);
	const struct realtek_intc_info *info = data->info;

	writel(CLEAN_INTC_STATUS, data->base + info->umsk_isr_offset);
	writel(CLEAN_INTC_STATUS, data->base + info->isr_offset);
	writel(data->saved_en, data->base + info->scpu_int_en_offset);

	return 0;
}

static const struct dev_pm_ops realtek_intc_rtd1319_pm_ops = {
	.suspend_noirq = realtek_intc_rtd1319_suspend,
	.resume_noirq  = realtek_intc_rtd1319_resume,
};

static int rtd1319_intc_probe(struct platform_device *pdev)
{
	const struct realtek_intc_info *info;

	info = of_device_get_match_data(&pdev->dev);
	if (!info)
		return -EINVAL;

	return realtek_intc_probe(pdev, info);
}

static struct platform_driver realtek_intc_rtd1319_driver = {
	.probe = rtd1319_intc_probe,
	.driver = {
		.name = "realtek_intc_rtd1319",
		.of_match_table = realtek_intc_rtd1319_dt_matches,
		.suppress_bind_attrs = true,
		.pm = &realtek_intc_rtd1319_pm_ops,
	},
};

static int __init realtek_intc_rtd1319_init(void)
{
	return platform_driver_register(&realtek_intc_rtd1319_driver);
}
core_initcall(realtek_intc_rtd1319_init);

static void __exit realtek_intc_rtd1319_exit(void)
{
	platform_driver_unregister(&realtek_intc_rtd1319_driver);
}
module_exit(realtek_intc_rtd1319_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Realtek RTD1319 Interrupt Controller Driver");
