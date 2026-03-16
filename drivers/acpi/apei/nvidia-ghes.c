// SPDX-License-Identifier: GPL-2.0-only
/*
 * NVIDIA GHES vendor record handler
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>
#include <acpi/ghes.h>

static const guid_t nvidia_sec_guid =
	GUID_INIT(0x6d5244f2, 0x2712, 0x11ec,
		  0xbe, 0xa7, 0xcb, 0x3f, 0xdb, 0x95, 0xc7, 0x86);

#define NVIDIA_CPER_REG_PAIR_SIZE	16	/* address + value, each u64 */

struct cper_sec_nvidia {
	char	signature[16];
	__le16	error_type;
	__le16	error_instance;
	u8	severity;
	u8	socket;
	u8	number_regs;
	u8	reserved;
	__le64	instance_base;
} __packed;

struct nvidia_ghes_private {
	struct notifier_block	nb;
	struct device		*dev;
};

static void nvidia_ghes_print_error(struct device *dev,
				    const struct cper_sec_nvidia *nvidia_err,
				    size_t error_data_length, bool fatal)
{
	const char *level = fatal ? KERN_ERR : KERN_INFO;
	const u8 *reg_data;
	size_t min_size;
	int i;

	dev_printk(level, dev, "signature: %.16s\n", nvidia_err->signature);
	dev_printk(level, dev, "error_type: %u\n", le16_to_cpu(nvidia_err->error_type));
	dev_printk(level, dev, "error_instance: %u\n", le16_to_cpu(nvidia_err->error_instance));
	dev_printk(level, dev, "severity: %u\n", nvidia_err->severity);
	dev_printk(level, dev, "socket: %u\n", nvidia_err->socket);
	dev_printk(level, dev, "number_regs: %u\n", nvidia_err->number_regs);
	dev_printk(level, dev, "instance_base: 0x%016llx\n",
		   (unsigned long long)le64_to_cpu(nvidia_err->instance_base));

	if (nvidia_err->number_regs == 0)
		return;

	/*
	 * Validate that all registers fit within error_data_length.
	 * Each register pair is NVIDIA_CPER_REG_PAIR_SIZE bytes (two u64s).
	 */
	min_size = sizeof(struct cper_sec_nvidia) +
		   (size_t)nvidia_err->number_regs * NVIDIA_CPER_REG_PAIR_SIZE;
	if (error_data_length < min_size) {
		dev_err(dev, "Invalid number_regs %u (section size %zu, need %zu)\n",
			nvidia_err->number_regs, error_data_length, min_size);
		return;
	}

	/*
	 * Registers are stored as address-value pairs immediately
	 * following the fixed header.  Each pair is two little-endian u64s.
	 */
	reg_data = (const u8 *)(nvidia_err + 1);
	for (i = 0; i < nvidia_err->number_regs; i++) {
		u64 addr = get_unaligned_le64(reg_data + i * NVIDIA_CPER_REG_PAIR_SIZE);
		u64 val = get_unaligned_le64(reg_data + i * NVIDIA_CPER_REG_PAIR_SIZE + 8);

		dev_printk(level, dev, "register[%d]: address=0x%016llx value=0x%016llx\n",
			   i, (unsigned long long)addr, (unsigned long long)val);
	}
}

static int nvidia_ghes_notify(struct notifier_block *nb,
			      unsigned long event, void *data)
{
	struct acpi_hest_generic_data *gdata = data;
	struct nvidia_ghes_private *priv;
	const struct cper_sec_nvidia *nvidia_err;
	guid_t sec_guid;

	import_guid(&sec_guid, gdata->section_type);
	if (!guid_equal(&sec_guid, &nvidia_sec_guid))
		return NOTIFY_DONE;

	priv = container_of(nb, struct nvidia_ghes_private, nb);

	if (acpi_hest_get_error_length(gdata) < sizeof(struct cper_sec_nvidia)) {
		dev_err(priv->dev, "Section too small (%u < %zu)\n",
			acpi_hest_get_error_length(gdata), sizeof(struct cper_sec_nvidia));
		return NOTIFY_OK;
	}

	nvidia_err = acpi_hest_get_payload(gdata);

	if (event >= GHES_SEV_RECOVERABLE)
		dev_err(priv->dev, "NVIDIA CPER section, error_data_length: %u\n",
			acpi_hest_get_error_length(gdata));
	else
		dev_info(priv->dev, "NVIDIA CPER section, error_data_length: %u\n",
			 acpi_hest_get_error_length(gdata));

	nvidia_ghes_print_error(priv->dev, nvidia_err, acpi_hest_get_error_length(gdata),
				event >= GHES_SEV_RECOVERABLE);

	return NOTIFY_OK;
}

static int nvidia_ghes_probe(struct platform_device *pdev)
{
	struct nvidia_ghes_private *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->nb.notifier_call = nvidia_ghes_notify;
	priv->dev = &pdev->dev;

	ret = ghes_register_vendor_record_notifier(&priv->nb);
	if (ret) {
		dev_err(&pdev->dev,
			"Failed to register NVIDIA GHES vendor record notifier: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, priv);

	return 0;
}

static void nvidia_ghes_remove(struct platform_device *pdev)
{
	struct nvidia_ghes_private *priv = platform_get_drvdata(pdev);

	ghes_unregister_vendor_record_notifier(&priv->nb);
}

static const struct acpi_device_id nvidia_ghes_acpi_match[] = {
	{ "NVDA2012", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, nvidia_ghes_acpi_match);

static struct platform_driver nvidia_ghes_driver = {
	.driver = {
		.name		= "nvidia-ghes",
		.acpi_match_table = nvidia_ghes_acpi_match,
	},
	.probe	= nvidia_ghes_probe,
	.remove	= nvidia_ghes_remove,
};
module_platform_driver(nvidia_ghes_driver);

MODULE_AUTHOR("Kai-Heng Feng <kaihengf@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA GHES vendor CPER record handler");
MODULE_LICENSE("GPL");
