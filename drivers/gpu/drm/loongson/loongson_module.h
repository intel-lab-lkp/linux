/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#ifndef __LOONGSON_MODULE_H__
#define __LOONGSON_MODULE_H__

#define DRIVER_AUTHOR               "Sui Jingfeng <sui.jingfeng@linux.dev>"
#define DRIVER_NAME                 "loongson"
#define DRIVER_DESC                 "drm driver for loongson graphics"
#define DRIVER_DATE                 "20220701"
#define DRIVER_MAJOR                1
#define DRIVER_MINOR                0
#define DRIVER_PATCHLEVEL           0

enum loongson_chip_id {
	CHIP_LS7A1000 = 0,
	CHIP_LS7A2000 = 1,
	CHIP_LS_LAST,
};

enum loongson_driver_type {
	LOONGSON_DRIVER_TYPE_UNKNOWN = 0,
	LOONGSON_DRIVER_TYPE_DRM_MASTER = 1,
	LOONGSON_DRIVER_TYPE_PCI = 2,
	LOONGSON_DRIVER_TYPE_PLATFORM = 4,
};

struct loongson_driver_info {
	struct device_driver *driver;
	u64 type;
};

const struct loongson_driver_info *loongson_get_driver_info_array(int *num);

extern int loongson_vblank;

extern struct pci_driver lsdc_pci_driver;
extern struct pci_driver loonggpu_pci_driver;
extern struct platform_driver lsdc_output_platform_driver;
extern struct platform_driver loongson_drm_platform_driver;

#endif
