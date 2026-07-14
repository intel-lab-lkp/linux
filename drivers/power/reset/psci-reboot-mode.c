// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/array_size.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/reboot-mode.h>
#include <linux/types.h>

static const struct reboot_mode_entry psci_resets[] = {
	{
		.name  = "psci-system-reset",
		.magic = { 0, PSCI_SYSTEM_RESET_COLD_RESET },
		.count = 2,
	},
	{
		.name  = "psci-system-reset2-arch-warm-reset",
		.magic = { 0, PSCI_SYSTEM_RESET2_ARCH_WARM_RESET },
		.count = 2,
	},
};

static u64 psci_reboot_mode_get_cookie(const u32 *magic, int count)
{
	u64 cookie = 0;
	int i;

	/* Build cookie from arg2/arg3 cells in order: cookie_hi then cookie_lo. */
	for (i = 1; i < count; i++)
		cookie = (cookie << 32) | magic[i];

	return cookie;
}

static int psci_reboot_mode_write(struct reboot_mode_driver *reboot,
				  u32 *magic, int count)
{
	if (count < 1 || count > 3)
		return -EINVAL;

	return psci_set_reset_cmd(magic[0], psci_reboot_mode_get_cookie(magic, count));
}

static int psci_reboot_mode_probe(struct platform_device *pdev)
{
	struct reboot_mode_driver *reboot;
	size_t count;
	int ret;

	reboot = devm_kzalloc(&pdev->dev, sizeof(*reboot), GFP_KERNEL);
	if (!reboot)
		return -ENOMEM;

	reboot_mode_driver_init(reboot, &pdev->dev, psci_reboot_mode_write);

	/* Skip PSCI SYSTEM_RESET2 modes if unsupported */
	count = psci_has_system_reset2_support() ? ARRAY_SIZE(psci_resets) : 1;
	ret = reboot_mode_add_predefined_modes(reboot, psci_resets, count);
	if (ret)
		return ret;

	return devm_reboot_mode_register(&pdev->dev, reboot);
}

static struct platform_driver psci_reboot_mode_driver = {
	.probe  = psci_reboot_mode_probe,
	.driver = {
		.name	= "psci-reboot-mode",
	},
};
module_platform_driver(psci_reboot_mode_driver);

MODULE_DESCRIPTION("PSCI reboot mode driver");
MODULE_LICENSE("GPL");
