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

static int __init loongson_module_init(void)
{
	int ret;

	if (!loongson_modeset || video_firmware_drivers_only())
		return -ENODEV;

	ret = platform_driver_register(&lsdc_output_port_platform_driver);
	if (ret)
		return ret;

	ret = pci_register_driver(&loong_gpu_pci_driver);
	if (ret) {
		platform_driver_unregister(&lsdc_output_port_platform_driver);
		return ret;
	}

	ret = pci_register_driver(&lsdc_pci_driver);
	if (ret) {
		pci_unregister_driver(&loong_gpu_pci_driver);
		platform_driver_unregister(&lsdc_output_port_platform_driver);
		return ret;
	}

	return 0;
}
module_init(loongson_module_init);

static void __exit loongson_module_exit(void)
{
	pci_unregister_driver(&lsdc_pci_driver);

	pci_unregister_driver(&loong_gpu_pci_driver);

	platform_driver_unregister(&lsdc_output_port_platform_driver);
}
module_exit(loongson_module_exit);
