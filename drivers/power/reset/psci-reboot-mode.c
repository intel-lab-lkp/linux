// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/reboot-mode.h>
#include <linux/types.h>

/*
 * Predefined modes:
 *   reset_type = 0
 *   cookie stored in magic[63:32]
 */
#define PSCI_PREDEF_MAGIC(cookie)	((cookie) * BIT_ULL(32))

static const struct reboot_mode_entry psci_resets[] = {
	{
		.name  = "psci-system-reset",
		.magic = PSCI_PREDEF_MAGIC(PSCI_RESET_TYPE_SYSTEM_RESET),
	},
	{
		.name  = "psci-system-reset2-arch-warm-reset",
		.magic = PSCI_PREDEF_MAGIC(PSCI_RESET_TYPE_SYSTEM_RESET2_ARCH_WARM),
	},
};

/*
 * magic is a pre-encoded value:
 *   reset_type in low 32 bits
 *   cookie in high 32 bits
 */
static int psci_reboot_mode_write(struct reboot_mode_driver *reboot, u64 magic)
{
	psci_set_reset_cmd(magic);
	return 0;
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

MODULE_LICENSE("GPL");
