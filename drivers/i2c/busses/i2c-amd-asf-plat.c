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

#include <linux/acpi.h>
#include <linux/i2c-smbus.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "i2c-piix4.h"

static const char *sb800_asf_port_name = " port 1";

struct amd_asf_dev {
	struct device *dev;
	struct i2c_adapter adap;
	struct sb800_mmio_cfg mmio_cfg;
	unsigned short port_addr;
};

static int amd_asf_probe(struct platform_device *pdev)
{
	struct resource_entry *rentry;
	struct amd_asf_dev *asf_dev;
	struct acpi_device *adev;
	LIST_HEAD(res_list);
	int ret;

	adev = ACPI_COMPANION(&pdev->dev);
	if (!adev)
		return dev_err_probe(&pdev->dev, -ENODEV, "Failed to get ASF device\n");

	asf_dev = devm_kzalloc(&pdev->dev, sizeof(*asf_dev), GFP_KERNEL);
	if (!asf_dev)
		return dev_err_probe(&pdev->dev, -ENOMEM, "Failed to allocate memory\n");

	asf_dev->dev = &pdev->dev;
	platform_set_drvdata(pdev, asf_dev);

	asf_dev->adap.owner = THIS_MODULE;
	asf_dev->mmio_cfg.use_mmio = true;
	asf_dev->adap.class = I2C_CLASS_HWMON;

	ret = acpi_dev_get_resources(adev, &res_list, NULL, NULL);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "Error getting ASF ACPI resource: %d\n", ret);

	list_for_each_entry(rentry, &res_list, node) {
		switch (resource_type(rentry->res)) {
		case IORESOURCE_IO:
			asf_dev->port_addr = rentry->res->start;
			break;
		default:
			dev_warn(&adev->dev, "Invalid ASF resource\n");
			break;
		}
	}

	acpi_dev_free_resource_list(&res_list);
	/* Set up the sysfs linkage to our parent device */
	asf_dev->adap.dev.parent = &pdev->dev;

	snprintf(asf_dev->adap.name, sizeof(asf_dev->adap.name),
		 "SMBus ASF adapter%s at %04x", sb800_asf_port_name, asf_dev->port_addr);

	i2c_set_adapdata(&asf_dev->adap, asf_dev);
	ret = i2c_add_adapter(&asf_dev->adap);
	if (ret) {
		release_region(asf_dev->port_addr, SMBIOSIZE);
		return ret;
	}

	return 0;
}

static void amd_asf_remove(struct platform_device *pdev)
{
	struct amd_asf_dev *dev = platform_get_drvdata(pdev);

	if (dev->port_addr) {
		i2c_del_adapter(&dev->adap);
		release_region(dev->port_addr, SMBIOSIZE);
	}
}

static const struct acpi_device_id amd_asf_acpi_ids[] = {
	{"AMDI001A", 0},
	{ }
};
MODULE_DEVICE_TABLE(acpi, amd_asf_acpi_ids);

static struct platform_driver amd_asf_driver = {
	.driver = {
		.name = "i2c-amd-asf",
		.acpi_match_table = amd_asf_acpi_ids,
	},
	.probe = amd_asf_probe,
	.remove_new = amd_asf_remove,
};
module_platform_driver(amd_asf_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMD Alert Standard Format Driver");
