// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/types.h>

/* Predefined modes use reset_type = 0 and cookie in magic[63:32]. */
#define PSCI_PREDEF_MAGIC(cookie)	((cookie) * BIT_ULL(32))

struct psci_predefined_reset {
	const char *mode;
	u64 magic;
};

static const struct psci_predefined_reset psci_resets[] = {
	{
		.mode = "psci-system-reset",
		.magic = PSCI_PREDEF_MAGIC(PSCI_RESET_TYPE_SYSTEM_RESET),
	},
	{
		.mode = "psci-system-reset2-arch-warm-reset",
		.magic = PSCI_PREDEF_MAGIC(PSCI_RESET_TYPE_SYSTEM_RESET2_ARCH_WARM),
	},
};

static int psci_reboot_mode_add_predefined_mode(struct device *dev,
						struct reboot_mode_driver *reboot,
						const struct psci_predefined_reset *predef)
{
	struct mode_info *info;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	INIT_LIST_HEAD(&info->list);
	info->mode = predef->mode;
	info->magic = predef->magic;
	list_add_tail(&info->list, &reboot->predefined_modes);

	return 0;
}

static int psci_reboot_mode_set_predefined_modes(struct device *dev,
						 struct reboot_mode_driver *reboot)
{
	int ret;

	INIT_LIST_HEAD(&reboot->predefined_modes);

	/* Always register psci-system-reset. */
	ret = psci_reboot_mode_add_predefined_mode(dev, reboot, &psci_resets[0]);
	if (ret)
		return ret;

	/* Register arch warm reset only if SYSTEM_RESET2 is supported. */
	if (!psci_has_system_reset2_support())
		return 0;

	return psci_reboot_mode_add_predefined_mode(dev, reboot, &psci_resets[1]);
}

/*
 * Pass the encoded magic to psci_set_reset_cmd.
 * magic is encoded as reset_type (low 32 bits) and cookie (high 32 bits).
 */
static int psci_reboot_mode_write(struct reboot_mode_driver *reboot, u64 magic)
{
	psci_set_reset_cmd(magic);
	return 0;
}

static int psci_reboot_mode_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reboot_mode_driver *reboot;
	int ret;

	reboot = devm_kzalloc(dev, sizeof(*reboot), GFP_KERNEL);
	if (!reboot)
		return -ENOMEM;

	ret = psci_reboot_mode_set_predefined_modes(dev, reboot);
	if (ret)
		return ret;

	reboot->write = psci_reboot_mode_write;
	reboot->dev = dev;

	return devm_reboot_mode_register(dev, reboot);
}

static struct platform_driver psci_reboot_mode_driver = {
	.probe  = psci_reboot_mode_probe,
	.driver = {
		.name	= "psci-reboot-mode",
	},
};

module_platform_driver(psci_reboot_mode_driver);

MODULE_LICENSE("GPL");
