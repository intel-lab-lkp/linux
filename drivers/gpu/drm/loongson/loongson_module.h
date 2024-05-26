/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#ifndef __LOONGSON_MODULE_H__
#define __LOONGSON_MODULE_H__

enum loongson_chip_id {
	CHIP_LS7A1000 = 0,
	CHIP_LS7A2000 = 1,
	CHIP_LS_LAST,
};

extern int loongson_vblank;
extern struct pci_driver lsdc_pci_driver;
extern struct pci_driver loong_gpu_pci_driver;
extern struct platform_driver lsdc_output_port_platform_driver;

#endif
