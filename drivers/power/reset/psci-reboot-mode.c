// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/device/faux.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/psci.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/types.h>

/*
 * Predefined reboot-modes:
 * reset_type(arg1) is zero; cookie(arg2) is stored in magic.
 * psci_reboot_mode_set_predefined_modes to move values to higher 32 bit of magic.
 */
static struct mode_info psci_resets[] = {
	{ .mode = "warm", .magic = REBOOT_WARM},
	{ .mode = "soft", .magic = REBOOT_SOFT},
	{ .mode = "cold", .magic = REBOOT_COLD},
};

static void psci_reboot_mode_set_predefined_modes(struct reboot_mode_driver *reboot)
{
	INIT_LIST_HEAD(&reboot->predefined_modes);
	for (u32 i = 0; i < ARRAY_SIZE(psci_resets); i++) {
		/* Move values to higher 32 bit of magic */
		psci_resets[i].magic = FIELD_PREP(GENMASK_ULL(63, 32), psci_resets[i].magic);
		INIT_LIST_HEAD(&psci_resets[i].list);
		list_add_tail(&psci_resets[i].list, &reboot->predefined_modes);
	}
}

/*
 * magic is 64 bit.
 * arg1 - reset_type(Low 32 bit of magic).
 * arg2 - cookie(High 32 bit of magic).
 * arg2(cookie) decides the mode, If arg1(reset_type) is 0;
 */
static int psci_reboot_mode_write(struct reboot_mode_driver *reboot, u64 magic)
{
	u32 reset_type = FIELD_GET(GENMASK_ULL(31, 0), magic);
	u32 cookie = FIELD_GET(GENMASK_ULL(63, 32), magic);

	if (reset_type == 0) {
		if (cookie == REBOOT_WARM || cookie == REBOOT_SOFT)
			psci_set_reset_cmd(true, 0, 0);
		else
			psci_set_reset_cmd(false, 0, 0);
	} else {
		psci_set_reset_cmd(true, reset_type, cookie);
	}

	return NOTIFY_DONE;
}

static int psci_reboot_mode_probe(struct faux_device *fdev)
{
	struct reboot_mode_driver *reboot;
	struct device_node *psci_np;
	struct device_node *np;
	int ret;

	psci_np = of_find_compatible_node(NULL, NULL, "arm,psci-1.0");
	if (!psci_np)
		return -ENODEV;

	/*
	 * Find the psci:reboot-mode node.
	 * If NULL, continue to register predefined modes.
	 * np refcount to be handled by dev;
	 * psci_np refcount is decremented by of_find_node_by_name;
	 */
	np = of_find_node_by_name(psci_np, "reboot-mode");
	fdev->dev.of_node = np;

	reboot = devm_kzalloc(&fdev->dev, sizeof(*reboot), GFP_KERNEL);
	if (!reboot)
		return -ENOMEM;

	psci_reboot_mode_set_predefined_modes(reboot);
	reboot->write = psci_reboot_mode_write;
	reboot->dev = &fdev->dev;

	ret = devm_reboot_mode_register(&fdev->dev, reboot);
	if (ret) {
		dev_err(&fdev->dev, "devm_reboot_mode_register failed %d\n", ret);
		return ret;
	}

	return 0;
}

static struct faux_device_ops psci_reboot_mode_ops = {
	.probe = psci_reboot_mode_probe,
};

static int __init psci_reboot_mode_init(void)
{
	struct faux_device *fdev;

	fdev = faux_device_create("psci-reboot-mode", NULL, &psci_reboot_mode_ops);
	if (!fdev)
		return -ENODEV;

	return 0;
}
device_initcall(psci_reboot_mode_init);
