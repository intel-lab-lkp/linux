/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD Platform Management Framework Interface
 *
 * Copyright (c) 2023, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Shyam Sundar S K <Shyam-sundar.S-k@amd.com>
 */

#ifndef AMD_PMF_IO_H
#define AMD_PMF_IO_H

#include <acpi/video.h>
#include <drm/drm_connector.h>
#include <linux/backlight.h>
#include <linux/thermal.h>

#define MAX_SUPPORTED 4

/* amdgpu */
struct amd_gpu_pmf_data {
	struct pci_dev *gpu_dev;
	struct backlight_device *raw_bd;
	struct thermal_cooling_device *cooling_dev;
	enum drm_connector_status con_status[MAX_SUPPORTED];
	int display_count;
	int connector_type[MAX_SUPPORTED];
	bool gpu_dev_en;
};

int amd_pmf_get_gfx_data(struct amd_gpu_pmf_data *pmf);
int amd_pmf_gpu_init(struct amd_gpu_pmf_data *pmf);
void amd_pmf_gpu_deinit(struct amd_gpu_pmf_data *pmf);
#endif
