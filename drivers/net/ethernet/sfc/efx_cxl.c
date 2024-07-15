// SPDX-License-Identifier: GPL-2.0-only
/****************************************************************************
 * Driver for AMD network controllers and boards
 * Copyright (C) 2024, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */


#include <linux/pci.h>
#include <linux/cxl_accel_mem.h>
#include <linux/cxl_accel_pci.h>

#include "net_driver.h"
#include "efx_cxl.h"

#define EFX_CTPIO_BUFFER_SIZE	(1024*1024*256)

void efx_cxl_init(struct efx_nic *efx)
{
	struct pci_dev *pci_dev = efx->pci_dev;
	struct efx_cxl *cxl = efx->cxl;
	struct resource res;
	u16 dvsec;

	dvsec = pci_find_dvsec_capability(pci_dev, PCI_VENDOR_ID_CXL,
					  CXL_DVSEC_PCIE_DEVICE);

	if (!dvsec)
		return;

	pci_info(pci_dev, "CXL CXL_DVSEC_PCIE_DEVICE capability found");

	cxl->cxlds = cxl_accel_state_create(&pci_dev->dev);
	if (IS_ERR(cxl->cxlds)) {
		pci_info(pci_dev, "CXL accel device state failed");
		return;
	}

	cxl_accel_set_dvsec(cxl->cxlds, dvsec);
	cxl_accel_set_serial(cxl->cxlds, pci_dev->dev.id);

	res = DEFINE_RES_MEM(0, EFX_CTPIO_BUFFER_SIZE);
	cxl_accel_set_resource(cxl->cxlds, res, CXL_ACCEL_RES_DPA);

	res = DEFINE_RES_MEM_NAMED(0, EFX_CTPIO_BUFFER_SIZE, "ram");
	cxl_accel_set_resource(cxl->cxlds, res, CXL_ACCEL_RES_RAM);
}


MODULE_IMPORT_NS(CXL);
