// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include <linux/pci.h>
#include <linux/platform_device.h>

#include <video/nomodeset.h>

#include "loongson_module.h"

static int loongson_modeset = -1;
MODULE_PARM_DESC(modeset, "Disable/Enable modesetting");
module_param_named(modeset, loongson_modeset, int, 0400);

int loongson_vblank = 1;
MODULE_PARM_DESC(vblank, "Disable/Enable hw vblank support");
module_param_named(vblank, loongson_vblank, int, 0400);

static const struct loongson_driver_info loongson_driver_array[] = {
	{
		.driver = &lsdc_output_platform_driver.driver,
		.type = LOONGSON_DRIVER_TYPE_PLATFORM,
	},
	{
		.driver = &lsdc_pci_driver.driver,
		.type = LOONGSON_DRIVER_TYPE_PCI,
	},
	{
		.driver = &loonggpu_pci_driver.driver,
		.type = LOONGSON_DRIVER_TYPE_PCI,
	},
	{
		.driver = &loongson_drm_platform_driver.driver,
		.type = LOONGSON_DRIVER_TYPE_PLATFORM | LOONGSON_DRIVER_TYPE_DRM_MASTER,
	},
	{
		.driver = NULL,
		.type = LOONGSON_DRIVER_TYPE_UNKNOWN,
	},
};

const struct loongson_driver_info *loongson_get_driver_info_array(int *num)
{
	if (num)
		*num = ARRAY_SIZE(loongson_driver_array) - 1;

	return loongson_driver_array;
}

static inline void loongson_unregister_driver(int count)
{
	const struct loongson_driver_info *ldi;
	struct device_driver *driver;

	while (count-- > 0) {
		ldi = &loongson_driver_array[count];
		driver = ldi->driver;
		if (!driver)
			continue;

		if (ldi->type & LOONGSON_DRIVER_TYPE_PCI)
			pci_unregister_driver(to_pci_driver(driver));
		else if (ldi->type & LOONGSON_DRIVER_TYPE_PLATFORM)
			platform_driver_unregister(to_platform_driver(driver));
	}
}

static int __init loongson_module_init(void)
{
	const struct loongson_driver_info *ldi = loongson_driver_array;
	int count = 0;
	int ret;

	if (!loongson_modeset || video_firmware_drivers_only())
		return -ENODEV;

	while (ldi->driver) {
		if (ldi->type & LOONGSON_DRIVER_TYPE_PCI) {
			ret = pci_register_driver(to_pci_driver(ldi->driver));
			if (ret)
				goto register_driver_err;
			goto register_driver_ok;
		}

		if (ldi->type & LOONGSON_DRIVER_TYPE_PLATFORM) {
			ret = platform_driver_register(to_platform_driver(ldi->driver));
			if (ret)
				goto register_driver_err;
			goto register_driver_ok;
		}

register_driver_ok:
		++count;
		++ldi;
	}

	pr_info("loongson: total %d drivers registered\n", count);
	return 0;

register_driver_err:
	loongson_unregister_driver(count);

	return ret;
}
module_init(loongson_module_init);

static void __exit loongson_module_exit(void)
{
	loongson_unregister_driver(ARRAY_SIZE(loongson_driver_array) - 1);
}
module_exit(loongson_module_exit);
