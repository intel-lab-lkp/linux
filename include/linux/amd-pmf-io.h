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

#include <drm/drm_connector.h>

#define MAX_SUPPORTED 4

/* amdgpu */
struct amd_gpu_pmf_data {
	struct pci_dev *gpu_dev;
	enum drm_connector_status con_status[MAX_SUPPORTED];
	int display_count;
	int connector_type[MAX_SUPPORTED];
	int brightness;
};

int amd_pmf_get_gfx_data(struct amd_gpu_pmf_data *pmf);
int amd_pmf_set_gfx_data(struct amd_gpu_pmf_data *pmf);
#endif
