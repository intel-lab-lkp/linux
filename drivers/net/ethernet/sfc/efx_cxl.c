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
	resource_size_t max;
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

	cxl->cxlmd = devm_cxl_add_memdev(&pci_dev->dev, cxl->cxlds);
	if (IS_ERR(cxl->cxlmd)) {
		pci_err(pci_dev, "CXL accel memdev creation failed");
		rc = PTR_ERR(cxl->cxlmd);
		goto err;
	}

	cxl->endpoint = cxl_acquire_endpoint(cxl->cxlmd);
	if (IS_ERR(cxl->endpoint)) {
		rc = PTR_ERR(cxl->endpoint);
		if (rc != -EPROBE_DEFER) {
			pci_err(pci_dev, "CXL accel acquire endpoint failed");
			goto err;
		}
	}

	cxl->cxlrd = cxl_get_hpa_freespace(cxl->endpoint,
					   CXL_DECODER_F_RAM | CXL_DECODER_F_TYPE2,
					   &max);

	if (IS_ERR(cxl->cxlrd)) {
		pci_err(pci_dev, "cxl_get_hpa_freespace failed\n");
		rc = PTR_ERR(cxl->cxlrd);
		goto err_release;
	}

	if (max < EFX_CTPIO_BUFFER_SIZE) {
		pci_err(pci_dev, "%s: no enough free HPA space %llu < %u\n",
			__func__, max, EFX_CTPIO_BUFFER_SIZE);
		rc = -ENOSPC;
		goto err;
	}

	cxl->cxled = cxl_request_dpa(cxl->endpoint, true, EFX_CTPIO_BUFFER_SIZE,
				     EFX_CTPIO_BUFFER_SIZE);
	if (IS_ERR(cxl->cxled)) {
		pci_err(pci_dev, "CXL accel request DPA failed");
		rc = PTR_ERR(cxl->cxlrd);
		goto err_release;
	}

	cxl->efx_region = cxl_create_region(cxl->cxlrd, cxl->cxled);
	if (!cxl->efx_region) {
		pci_err(pci_dev, "CXL accel create region failed");
		rc = PTR_ERR(cxl->efx_region);
		goto err_region;
	}

	cxl_release_endpoint(cxl->cxlmd, cxl->endpoint);

	return 0;

err_region:
	cxl_dpa_free(efx->cxl->cxled);
err_release:
	cxl_release_endpoint(cxl->cxlmd, cxl->endpoint);
err:
	kfree(cxl->cxlds);
	kfree(cxl);
	efx->cxl = NULL;

	return rc;
}

void efx_cxl_exit(struct efx_nic *efx)
{
	if (efx->cxl) {
		cxl_region_detach(efx->cxl->cxled);
		cxl_dpa_free(efx->cxl->cxled);
		cxl_release_resource(efx->cxl->cxlds, CXL_ACCEL_RES_RAM);
		kfree(efx->cxl->cxlds);
		kfree(efx->cxl);
	}
}

MODULE_IMPORT_NS(CXL);
