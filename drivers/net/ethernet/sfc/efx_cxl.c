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

#include <cxl/pci.h>
#include <linux/pci.h>

#include "net_driver.h"
#include "efx_cxl.h"

#define EFX_CTPIO_BUFFER_SIZE	SZ_256M

int efx_cxl_init(struct efx_probe_data *probe_data)
{
	DECLARE_BITMAP(expected, CXL_MAX_CAPS) = {};
	DECLARE_BITMAP(found, CXL_MAX_CAPS) = {};
	struct efx_nic *efx = &probe_data->efx;
	struct pci_dev *pci_dev = efx->pci_dev;
	struct cxl_dpa_info sfc_dpa_info = {
		.size = EFX_CTPIO_BUFFER_SIZE
	};
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
	cxl = cxl_dev_state_create(&pci_dev->dev, CXL_DEVTYPE_DEVMEM,
				   pci_dev->dev.id, dvsec, struct efx_cxl,
				   cxlds, false);

	if (!cxl)
		return -ENOMEM;

	set_bit(CXL_DEV_CAP_HDM, expected);
	set_bit(CXL_DEV_CAP_RAS, expected);

	rc = cxl_pci_accel_setup_regs(pci_dev, &cxl->cxlds, found);
	if (rc) {
		pci_err(pci_dev, "CXL accel setup regs failed");
		return rc;
	}

	/*
	 * Checking mandatory caps are there as, at least, a subset of those
	 * found.
	 */
	if (cxl_check_caps(pci_dev, expected, found))
		return -ENXIO;

	/*
	 * Set media ready explicitly as there are neither mailbox for checking
	 * this state nor the CXL register involved, both not mandatory for
	 * type2.
	 */
	cxl->cxlds.media_ready = true;

	cxl_mem_dpa_init(&sfc_dpa_info, EFX_CTPIO_BUFFER_SIZE, 0);
	rc = cxl_dpa_setup(&cxl->cxlds, &sfc_dpa_info);
	if (rc)
		return rc;

	cxl->cxlmd = devm_cxl_add_memdev(&pci_dev->dev, &cxl->cxlds);

	if (IS_ERR(cxl->cxlmd)) {
		pci_err(pci_dev, "CXL accel memdev creation failed");
		return PTR_ERR(cxl->cxlmd);
	}

	cxl->cxlrd = cxl_get_hpa_freespace(cxl->cxlmd, 1,
					   CXL_DECODER_F_RAM | CXL_DECODER_F_TYPE2,
					   &max_size);

	if (IS_ERR(cxl->cxlrd)) {
		pci_err(pci_dev, "cxl_get_hpa_freespace failed\n");
		return PTR_ERR(cxl->cxlrd);
	}

	if (max_size < EFX_CTPIO_BUFFER_SIZE) {
		pci_err(pci_dev, "%s: not enough free HPA space %pap < %u\n",
			__func__, &max_size, EFX_CTPIO_BUFFER_SIZE);
		cxl_put_root_decoder(cxl->cxlrd);
		rc = -ENOSPC;
		goto sfc_put_decoder;
	}

	cxl->cxled = cxl_request_dpa(cxl->cxlmd, CXL_PARTMODE_RAM,
				     EFX_CTPIO_BUFFER_SIZE);
	if (IS_ERR(cxl->cxled)) {
		pci_err(pci_dev, "CXL accel request DPA failed");
		rc = PTR_ERR(cxl->cxled);
		goto sfc_put_decoder;
	}

	cxl->efx_region = cxl_create_region(cxl->cxlrd, cxl->cxled, 1, true);
	if (IS_ERR(cxl->efx_region)) {
		pci_err(pci_dev, "CXL accel create region failed");
		rc = PTR_ERR(cxl->efx_region);
		goto err_region;
	}

	probe_data->cxl = cxl;

	return 0;

err_region:
	cxl_dpa_free(cxl->cxled);
sfc_put_decoder:
	cxl_put_root_decoder(cxl->cxlrd);
	return rc;
}

void efx_cxl_exit(struct efx_probe_data *probe_data)
{
	if (probe_data->cxl) {
		cxl_accel_region_detach(probe_data->cxl->cxled);
		cxl_dpa_free(probe_data->cxl->cxled);
		cxl_put_root_decoder(probe_data->cxl->cxlrd);
	}
}

MODULE_IMPORT_NS("CXL");
