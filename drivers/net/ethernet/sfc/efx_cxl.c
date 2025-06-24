// SPDX-License-Identifier: GPL-2.0-only
/****************************************************************************
 *
 * Driver for AMD network controllers and boards
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#include <cxl/cxl.h>
#include <cxl/pci.h>
#include <linux/pci.h>

#include "net_driver.h"
#include "efx_cxl.h"

#define EFX_CTPIO_BUFFER_SIZE	SZ_256M

int efx_cxl_init(struct efx_probe_data *probe_data)
{
	struct efx_nic *efx = &probe_data->efx;
	struct pci_dev *pci_dev = efx->pci_dev;
	resource_size_t max_size;
	struct efx_cxl *cxl;
	u16 dvsec;
	int rc;

	probe_data->cxl_pio_initialised = false;

	dvsec = pci_find_dvsec_capability(pci_dev, PCI_VENDOR_ID_CXL,
					  CXL_DVSEC_PCIE_DEVICE);
	if (!dvsec)
		return 0;

	pci_dbg(pci_dev, "CXL_DVSEC_PCIE_DEVICE capability found\n");

	/* Create a cxl_dev_state embedded in the cxl struct using cxl core api
	 * specifying no mbox available.
	 */
	cxl = devm_cxl_dev_state_create(&pci_dev->dev, CXL_DEVTYPE_DEVMEM,
					pci_dev->dev.id, dvsec, struct efx_cxl,
					cxlds, false);

	if (!cxl)
		return -ENOMEM;

	rc = cxl_pci_setup_regs(pci_dev, CXL_REGLOC_RBI_COMPONENT,
				&cxl->cxlds.reg_map);
	if (rc) {
		dev_warn(&pci_dev->dev, "No component registers (err=%d)\n", rc);
		return rc;
	}

	if (!cxl->cxlds.reg_map.component_map.hdm_decoder.valid) {
		dev_err(&pci_dev->dev, "Expected HDM component register not found\n");
		return -ENODEV;
	}

	if (!cxl->cxlds.reg_map.component_map.ras.valid) {
		dev_err(&pci_dev->dev, "Expected RAS component register not found\n");
		return -ENODEV;
	}

	rc = cxl_map_component_regs(&cxl->cxlds.reg_map,
				    &cxl->cxlds.regs.component,
				    BIT(CXL_CM_CAP_CAP_ID_RAS));
	if (rc) {
		dev_err(&pci_dev->dev, "Failed to map RAS capability.\n");
		return rc;
	}

	/*
	 * Set media ready explicitly as there are neither mailbox for checking
	 * this state nor the CXL register involved, both not mandatory for
	 * type2.
	 */
	cxl->cxlds.media_ready = true;

	cxl_set_capacity(&cxl->cxlds, EFX_CTPIO_BUFFER_SIZE);

	cxl->cxlmd = devm_cxl_add_memdev(&pci_dev->dev, &cxl->cxlds);

	if (IS_ERR(cxl->cxlmd)) {
		pci_err(pci_dev, "CXL accel memdev creation failed");
		return PTR_ERR(cxl->cxlmd);
	}

	cxl->endpoint = cxl_acquire_endpoint(cxl->cxlmd);
	if (IS_ERR(cxl->endpoint))
		return PTR_ERR(cxl->endpoint);

	cxl->cxlrd = cxl_get_hpa_freespace(cxl->cxlmd, 1,
					   CXL_DECODER_F_RAM | CXL_DECODER_F_TYPE2,
					   &max_size);

	if (IS_ERR(cxl->cxlrd)) {
		pci_err(pci_dev, "cxl_get_hpa_freespace failed\n");
		rc = PTR_ERR(cxl->cxlrd);
		goto endpoint_release;
	}

	if (max_size < EFX_CTPIO_BUFFER_SIZE) {
		pci_err(pci_dev, "%s: not enough free HPA space %pap < %u\n",
			__func__, &max_size, EFX_CTPIO_BUFFER_SIZE);
		rc = -ENOSPC;
		goto put_root_decoder;
	}

	probe_data->cxl = cxl;

	goto endpoint_release;

put_root_decoder:
	cxl_put_root_decoder(cxl->cxlrd);
endpoint_release:
	cxl_release_endpoint(cxl->cxlmd, cxl->endpoint);
	return rc;
}

void efx_cxl_exit(struct efx_probe_data *probe_data)
{
	if (probe_data->cxl)
		cxl_put_root_decoder(probe_data->cxl->cxlrd);
}

MODULE_IMPORT_NS("CXL");
