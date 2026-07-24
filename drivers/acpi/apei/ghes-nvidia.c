// SPDX-License-Identifier: GPL-2.0-only
/*
 * NVIDIA GHES vendor record handler
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <acpi/ghes.h>

#include <kunit/visibility.h>
#include "ghes-nvidia.h"

static const guid_t nvidia_grace_sec_guid =
	GUID_INIT(0x6d5244f2, 0x2712, 0x11ec,
		  0xbe, 0xa7, 0xcb, 0x3f, 0xdb, 0x95, 0xc7, 0x86);

struct cper_sec_nvidia {
	char	signature[16];
	__le16	error_type;
	__le16	error_instance;
	u8	severity;
	u8	socket;
	u8	number_regs;
	u8	reserved;
	__le64	instance_base;
	struct nvidia_ghes_grace_reg regs[] __counted_by(number_regs);
};

struct nvidia_ghes_private {
	struct notifier_block	nb;
	struct device		*dev;
};

VISIBLE_IF_KUNIT
int nvidia_ghes_decode_grace(struct device *dev, const void *buf,
			     size_t len,
			     struct nvidia_ghes_decoded *decoded)
{
	const struct cper_sec_nvidia *nvidia_err = buf;
	size_t min_size;

	if (!buf || !decoded)
		return -EINVAL;
	if (len < sizeof(*nvidia_err)) {
		if (dev)
			dev_err(dev, "Section too small (%zu < %zu)\n",
				len, sizeof(*nvidia_err));
		return -ENODATA;
	}

	min_size = struct_size(nvidia_err, regs, nvidia_err->number_regs);
	if (len < min_size) {
		if (dev)
			dev_err(dev,
				"Invalid number_regs %u (section size %zu, need %zu)\n",
				nvidia_err->number_regs, len, min_size);
		return -ENODATA;
	}

	memset(decoded, 0, sizeof(*decoded));
	decoded->format = NVIDIA_GHES_FORMAT_GRACE;
	memcpy(decoded->signature, nvidia_err->signature, sizeof(nvidia_err->signature));
	decoded->signature[sizeof(nvidia_err->signature)] = '\0';
	decoded->error_type = le16_to_cpu(nvidia_err->error_type);
	decoded->error_instance = le16_to_cpu(nvidia_err->error_instance);
	decoded->severity = nvidia_err->severity;
	decoded->socket = nvidia_err->socket;
	decoded->number_regs = nvidia_err->number_regs;
	decoded->instance_base = le64_to_cpu(nvidia_err->instance_base);
	if (nvidia_err->number_regs)
		decoded->grace_regs = nvidia_err->regs;

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_decode_grace);

VISIBLE_IF_KUNIT
int nvidia_ghes_grace_reg_pair(const struct nvidia_ghes_decoded *decoded,
				      unsigned int index, u64 *addr, u64 *val)
{
	const struct nvidia_ghes_grace_reg *regs;

	if (!decoded || decoded->format != NVIDIA_GHES_FORMAT_GRACE || !addr || !val)
		return -EINVAL;
	if (index >= decoded->number_regs)
		return -ERANGE;

	regs = decoded->grace_regs;
	*addr = le64_to_cpu(regs[index].addr);
	*val = le64_to_cpu(regs[index].val);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_grace_reg_pair);

static void nvidia_ghes_print_grace(struct device *dev,
				    const struct nvidia_ghes_decoded *decoded,
				    bool fatal)
{
	const char *level = fatal ? KERN_ERR : KERN_INFO;
	u64 addr, val;

	dev_printk(level, dev, "signature: %s\n", decoded->signature);
	dev_printk(level, dev, "error_type: %u\n", decoded->error_type);
	dev_printk(level, dev, "error_instance: %u\n", decoded->error_instance);
	dev_printk(level, dev, "severity: %u\n", decoded->severity);
	dev_printk(level, dev, "socket: %u\n", decoded->socket);
	dev_printk(level, dev, "number_regs: %u\n", decoded->number_regs);
	dev_printk(level, dev, "instance_base: 0x%016llx\n", decoded->instance_base);

	for (int i = 0; i < decoded->number_regs; i++) {
		if (nvidia_ghes_grace_reg_pair(decoded, i, &addr, &val))
			break;
		dev_printk(level, dev, "register[%d]: address=0x%016llx value=0x%016llx\n",
			   i, addr, val);
	}
}

static int nvidia_ghes_notify(struct notifier_block *nb,
			      unsigned long event, void *data)
{
	struct acpi_hest_generic_data *gdata = data;
	struct nvidia_ghes_decoded decoded;
	struct nvidia_ghes_private *priv;
	const void *payload;
	guid_t sec_guid;
	u32 len;
	int ret;
	bool fatal;

	import_guid(&sec_guid, gdata->section_type);
	if (!guid_equal(&sec_guid, &nvidia_grace_sec_guid))
		return NOTIFY_DONE;

	priv = container_of(nb, struct nvidia_ghes_private, nb);
	len = acpi_hest_get_error_length(gdata);
	payload = acpi_hest_get_payload(gdata);
	fatal = event >= GHES_SEV_RECOVERABLE;

	ret = nvidia_ghes_decode_grace(priv->dev, payload, len, &decoded);
	if (ret) {
		dev_err(priv->dev,
			"Malformed NVIDIA CPER section, error_data_length: %u, ret: %d\n",
			len, ret);
		return NOTIFY_OK;
	}

	dev_printk(fatal ? KERN_ERR : KERN_INFO, priv->dev,
		   "NVIDIA CPER section, error_data_length: %u\n", len);
	nvidia_ghes_print_grace(priv->dev, &decoded, fatal);

	return NOTIFY_OK;
}

static int nvidia_ghes_probe(struct platform_device *pdev)
{
	struct nvidia_ghes_private *priv;
	int ret;

	priv = devm_kmalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	*priv = (struct nvidia_ghes_private) {
		.nb.notifier_call = nvidia_ghes_notify,
		.dev = &pdev->dev,
	};

	ret = devm_ghes_register_vendor_record_notifier(&pdev->dev, &priv->nb);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register NVIDIA GHES vendor record notifier\n");

	return 0;
}

static const struct acpi_device_id nvidia_ghes_acpi_match[] = {
	{ "NVDA2012" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, nvidia_ghes_acpi_match);

static struct platform_driver nvidia_ghes_driver = {
	.driver = {
		.name = "nvidia-ghes",
		.acpi_match_table = nvidia_ghes_acpi_match,
	},
	.probe = nvidia_ghes_probe,
};
module_platform_driver(nvidia_ghes_driver);

MODULE_AUTHOR("Kai-Heng Feng <kaihengf@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA GHES vendor CPER record handler");
MODULE_LICENSE("GPL");
