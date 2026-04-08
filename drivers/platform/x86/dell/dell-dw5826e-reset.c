// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  dell-dw5826e-reset.c - Dell DW5826e reset driver
 *
 *  Copyright (C) 2026 Jackbb Wu <jackbb.wu@compal.com>
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

static guid_t palc_dsm_guid =
	GUID_INIT(0x5a1a4bba, 0x8006, 0x487e, 0xbe, 0x0a, 0xac, 0xf5, 0xd8, 0xfd, 0xfe, 0x59);

struct palc_dev {
	struct device *dev;
	acpi_handle handle;
	struct miscdevice miscdev;
};

static int trigger_palc_pldr(struct palc_dev *palc)
{
	union acpi_object *obj;

	dev_info(palc->dev, "Triggering PLDR via ACPI _DSM Function 1...\n");

	obj = acpi_evaluate_dsm(palc->handle, &palc_dsm_guid, 1, 1, NULL);

	if (!obj) {
		dev_err(palc->dev, "Failed to evaluate _DSM\n");
		return -EIO;
	}

	if (obj->type == ACPI_TYPE_BUFFER)
		dev_info(palc->dev, "PLDR _DSM executed successfully\n");

	ACPI_FREE(obj);
	return 0;
}

static ssize_t palc_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
	struct palc_dev *palc = filp->private_data;

	trigger_palc_pldr(palc);

	return count;
}

static int palc_open(struct inode *inode, struct file *filp)
{
	struct palc_dev *palc = container_of(filp->private_data, struct palc_dev, miscdev);

	filp->private_data = palc;
	return 0;
}

static const struct file_operations palc_fops = {
	.owner  = THIS_MODULE,
	.open   = palc_open,
	.write  = palc_write,
};

static int palc_acpi_probe(struct acpi_device *adev)
{
	struct palc_dev *palc;

	palc = devm_kzalloc(&adev->dev, sizeof(*palc), GFP_KERNEL);
	if (!palc)
		return -ENOMEM;

	palc->dev    = &adev->dev;
	palc->handle = adev->handle;

	palc->miscdev.minor  = MISC_DYNAMIC_MINOR;
	palc->miscdev.name   = "reset_palc";
	palc->miscdev.fops   = &palc_fops;
	palc->miscdev.parent = &adev->dev;

	if (misc_register(&palc->miscdev))
		return -EINVAL;

	dev_set_drvdata(&adev->dev, palc);

	dev_info(&adev->dev, "DW5826e Reset Device (PALC0001) Driver Loaded\n");
	return 0;
}

static void palc_acpi_remove(struct acpi_device *adev)
{
	struct palc_dev *palc = dev_get_drvdata(&adev->dev);

	if (palc)
		misc_deregister(&palc->miscdev);
}

static const struct acpi_device_id palc_acpi_ids[] = {
	{ "PALC0001", 0 },
	{ "", 0 }
};

static struct acpi_driver palc_acpi_driver = {
	.name  = "palc_reset",
	.ids   = palc_acpi_ids,
	.ops = {
		.add    = palc_acpi_probe,
		.remove = palc_acpi_remove,
	},
};

MODULE_DEVICE_TABLE(acpi, palc_acpi_ids);
module_acpi_driver(palc_acpi_driver);

MODULE_DESCRIPTION("Dell DW5826e reset driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("JackBB Wu");
