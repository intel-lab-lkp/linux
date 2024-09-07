// SPDX-License-Identifier: GPL-2.0-only
/****************************************************************************
 *
 * Driver for AMD network controllers and boards
 * Copyright (C) 2024, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#include <linux/cxl/cxl.h>
#include <linux/cxl/pci.h>
#include <linux/pci.h>

#include "net_driver.h"
#include "efx_cxl.h"

#define EFX_CTPIO_BUFFER_SIZE	(1024 * 1024 * 256)

int efx_cxl_init(struct efx_nic *efx)
{
	struct pci_dev *pci_dev = efx->pci_dev;
	struct efx_cxl *cxl;
	struct resource res;
	u16 dvsec;
	int rc;

	efx->efx_cxl_pio_initialised = false;

	dvsec = pci_find_dvsec_capability(pci_dev, PCI_VENDOR_ID_CXL,
					  CXL_DVSEC_PCIE_DEVICE);

	if (!dvsec)
		return 0;

	pci_dbg(pci_dev, "CXL_DVSEC_PCIE_DEVICE capability found\n");

	efx->cxl = kzalloc(sizeof(*cxl), GFP_KERNEL);
	if (!efx->cxl)
		return -ENOMEM;

	cxl = efx->cxl;

	cxl->cxlds = cxl_accel_state_create(&pci_dev->dev);
	if (IS_ERR(cxl->cxlds)) {
		pci_err(pci_dev, "CXL accel device state failed");
		kfree(efx->cxl);
		return -ENOMEM;
	}

	cxl_set_dvsec(cxl->cxlds, dvsec);
	cxl_set_serial(cxl->cxlds, pci_dev->dev.id);

	res = DEFINE_RES_MEM(0, EFX_CTPIO_BUFFER_SIZE);
	if (cxl_set_resource(cxl->cxlds, res, CXL_ACCEL_RES_DPA)) {
		pci_err(pci_dev, "cxl_set_resource DPA failed\n");
		rc = -EINVAL;
		goto err;
	}

	res = DEFINE_RES_MEM_NAMED(0, EFX_CTPIO_BUFFER_SIZE, "ram");
	if (cxl_set_resource(cxl->cxlds, res, CXL_ACCEL_RES_RAM)) {
		pci_err(pci_dev, "cxl_set_resource RAM failed\n");
		rc = -EINVAL;
		goto err;
	}

	rc = cxl_pci_accel_setup_regs(pci_dev, cxl->cxlds);
	if (rc) {
		pci_err(pci_dev, "CXL accel setup regs failed");
		goto err;
	}

	rc = cxl_request_resource(cxl->cxlds, CXL_ACCEL_RES_RAM);
	if (rc) {
		pci_err(pci_dev, "CXL request resource failed");
		goto err;
	}

	/* We do not have the register about media status. Hardware design
	 * implies it is ready.
	 */
	cxl_set_media_ready(cxl->cxlds);

	return 0;
err:
	kfree(cxl->cxlds);
	kfree(cxl);
	efx->cxl = NULL;

	return rc;
}

void efx_cxl_exit(struct efx_nic *efx)
{
	if (efx->cxl) {
		cxl_release_resource(efx->cxl->cxlds, CXL_ACCEL_RES_RAM);
		kfree(efx->cxl->cxlds);
		kfree(efx->cxl);
	}
}

MODULE_IMPORT_NS(CXL);
