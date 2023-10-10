// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for HiSilicon system timer driver.
 *
 * The device is fully compatible with ARM's Generic Timer specification.
 * The device is enumerated through DSDT rather than GTDT and can use
 * LPI interrupt besides SPI.
 *
 * Copyright (c) 2023 HiSilicon Technologies Co., Ltd.
 * Author: Yicong Yang <yangyicong@hisilicon.com>
 */
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/units.h>

#include <clocksource/arm_arch_timer.h>

#define DRV_NAME	"hisi_sys_timer"

#define CNTFRQ		0x10

static const struct acpi_device_id hisi_sys_timer_acpi_ids[] = {
	{ "HISI03F2", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, hisi_sys_timer_acpi_ids);

static int hisi_sys_timer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *iobase;
	int ret, irq;
	u32 freq;

	iobase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(iobase))
		return PTR_ERR(iobase);

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return dev_err_probe(dev, ret, "failed to get interrupt\n");

	ret = arch_timer_mem_register(iobase, irq, false, DRV_NAME);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register timer\n");

	freq = readl_relaxed(iobase + CNTFRQ);
	dev_info(dev, "%s works at %ldMHz\n", DRV_NAME, freq / HZ_PER_MHZ);
	return 0;
}

static struct platform_driver hisi_sys_timer_driver = {
	.probe = hisi_sys_timer_probe,
	.driver = {
		.name = DRV_NAME,
		.acpi_match_table = hisi_sys_timer_acpi_ids,
	},
};
module_platform_driver(hisi_sys_timer_driver);

MODULE_AUTHOR("Yicong Yang <yangyicong@hisilicon.com>");
MODULE_DESCRIPTION("HiSilicon system timer driver");
MODULE_LICENSE("GPL");
