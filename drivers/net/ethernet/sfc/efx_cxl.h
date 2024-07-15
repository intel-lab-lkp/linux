// SPDX-License-Identifier: GPL-2.0-only
/****************************************************************************
 * Driver for AMD network controllers and boards
 * Copyright (C) 2024, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#ifndef EFX_CXL_H
#define EFX_CLX_H

#include <linux/cxl_accel_mem.h>

struct efx_nic;

struct efx_cxl {
	cxl_accel_state *cxlds;
	struct cxl_memdev *cxlmd;
	struct cxl_root_decoder *cxlrd;
	struct cxl_port *endpoint;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_region *efx_region;
	void __iomem *ctpio_cxl;
};

void efx_cxl_init(struct efx_nic *efx);
void efx_cxl_exit(struct efx_nic *efx);
#endif
