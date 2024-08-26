// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for user clock monitor logic within Xilinx 'Clocking Wizard' IP core
 *
 * Copyright (C) 2024 Harry Austen <hpausten@protonmail.com>
 */

#include <linux/auxiliary_bus.h>
#include <linux/bits.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/uio_driver.h>

#define WZRD_INTR_ENABLE	0x10

static int clk_mon_irqcontrol(struct uio_info *info, s32 irq_on)
{
	if (irq_on)
		iowrite32(GENMASK(15, 0), info->mem[0].internal_addr + WZRD_INTR_ENABLE);
	else
		iowrite32(0, info->mem[0].internal_addr + WZRD_INTR_ENABLE);

	return 0;
}

static int probe(struct auxiliary_device *adev, const struct auxiliary_device_id *id)
{
	struct platform_device *pdev = to_platform_device(adev->dev.parent);
	struct device *dev = &adev->dev;
	struct uio_info *info;
	int irq;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return 0;

	info->name = KBUILD_MODNAME;
	info->version = "0.0.1";

	info->mem[0].name = "clock monitor";
	info->mem[0].memtype = UIO_MEM_PHYS;
	info->mem[0].addr = platform_get_resource(pdev, IORESOURCE_IO, 0)->start;
	info->mem[0].size = (WZRD_INTR_ENABLE + 4 + PAGE_SIZE - 1) & PAGE_MASK;
	info->mem[0].internal_addr = (__force void __iomem *)dev->platform_data;

	info->irq = irq;
	info->irqcontrol = clk_mon_irqcontrol;
	return devm_uio_register_device(dev, info);
}

static struct auxiliary_device_id ids[] = {
	{ .name = "clk_xlnx_clock_wizard.clk-mon" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, ids);

static struct auxiliary_driver xlnx_clk_mon_driver = {
	.id_table = ids,
	.probe = probe,
};

module_auxiliary_driver(xlnx_clk_mon_driver);

MODULE_AUTHOR("Harry Austen <hpausten@protonmail.com>");
MODULE_DESCRIPTION("Driver for Xilinx user clock monitor logic");
MODULE_LICENSE("GPL");
