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
 * Predefined reboot-modes are defined as per the values
 * of enum reboot_mode defined in the kernel: reboot.c.
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
		/* Prepare the magic with arg1 as 0 and arg2 as per pre-defined mode */
		psci_resets[i].magic = REBOOT_MODE_MAGIC(0, psci_resets[i].magic);
		INIT_LIST_HEAD(&psci_resets[i].list);
		list_add_tail(&psci_resets[i].list, &reboot->predefined_modes);
	}
}

/*
 * arg1 is reset_type(Low 32 bit of magic).
 * arg2 is cookie(High 32 bit of magic).
 * If reset_type is 0, cookie will be used to decide the reset command.
 */
static int psci_reboot_mode_write(struct reboot_mode_driver *reboot, u64 magic)
{
	u32 reset_type = REBOOT_MODE_ARG1(magic);
	u32 cookie = REBOOT_MODE_ARG2(magic);

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

static int psci_reboot_mode_register_device(struct faux_device *fdev)
{
	struct reboot_mode_driver *reboot;
	int ret;

	reboot = devm_kzalloc(&fdev->dev, sizeof(*reboot), GFP_KERNEL);
	if (!reboot)
		return -ENOMEM;

	psci_reboot_mode_set_predefined_modes(reboot);
	reboot->write = psci_reboot_mode_write;
	reboot->dev = &fdev->dev;

	ret = devm_reboot_mode_register(&fdev->dev, reboot);
	if (ret) {
		dev_err_probe(&fdev->dev, ret, "devm_reboot_mode_register failed %d\n", ret);
		return ret;
	}

	return 0;
}

static int __init psci_reboot_mode_init(void)
{
	struct device_node *psci_np;
	struct faux_device *fdev;
	struct device_node *np;
	int ret;

	psci_np = of_find_compatible_node(NULL, NULL, "arm,psci-1.0");
	if (!psci_np)
		return -ENODEV;
	/*
	 * Look for reboot-mode in the psci node. Even if the reboot-mode
	 * node is not defined in psci, continue to register with the
	 * reboot-mode driver and let the dev.ofnode be set as NULL.
	 */
	np = of_find_node_by_name(psci_np, "reboot-mode");

	fdev = faux_device_create("psci-reboot-mode", NULL, NULL);
	if (!fdev) {
		ret = -ENODEV;
		goto error;
	}

	device_set_node(&fdev->dev, of_fwnode_handle(np));
	ret = psci_reboot_mode_register_device(fdev);
	if (ret)
		goto error;

	return 0;

error:
	of_node_put(np);
	if (fdev) {
		device_set_node(&fdev->dev, NULL);
		faux_device_destroy(fdev);
	}

	return ret;
}
device_initcall(psci_reboot_mode_init);
