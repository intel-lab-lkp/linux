// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Alert Standard Format Platform Driver
 *
 * Copyright (c) 2024, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Authors: Shyam Sundar S K <Shyam-sundar.S-k@amd.com>
 *	    Sanket Goswami <Sanket.Goswami@amd.com>
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/sprintf.h>
#include "i2c-piix4.h"

static const char *amd_asf_port_name = " port 1";

struct amd_asf_dev {
	struct i2c_adapter adap;
	struct device *dev;
	struct sb800_mmio_cfg mmio_cfg;
	struct resource *port_addr;
};

static int amd_asf_probe(struct platform_device *pdev)
{
	struct amd_asf_dev *asf_dev;

	asf_dev = devm_kzalloc(&pdev->dev, sizeof(*asf_dev), GFP_KERNEL);
	if (!asf_dev)
		return dev_err_probe(&pdev->dev, -ENOMEM, "Failed to allocate memory\n");

	asf_dev->dev = &pdev->dev;
	asf_dev->mmio_cfg.use_mmio = true;
	platform_set_drvdata(pdev, asf_dev);

	asf_dev->port_addr = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!asf_dev->port_addr)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing IO resources\n");

	asf_dev->adap.owner = THIS_MODULE;
	asf_dev->adap.dev.parent = &pdev->dev;

	i2c_set_adapdata(&asf_dev->adap, asf_dev);
	snprintf(asf_dev->adap.name, sizeof(asf_dev->adap.name),
		 "SMBus ASF adapter%s at 0x%llx", amd_asf_port_name, asf_dev->port_addr->start);

	return devm_i2c_add_adapter(&pdev->dev, &asf_dev->adap);
}

static const struct acpi_device_id amd_asf_acpi_ids[] = {
	{ "AMDI001A" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, amd_asf_acpi_ids);

static struct platform_driver amd_asf_driver = {
	.driver = {
		.name = "i2c-amd-asf",
		.acpi_match_table = amd_asf_acpi_ids,
	},
	.probe = amd_asf_probe,
};
module_platform_driver(amd_asf_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMD Alert Standard Format Driver");
