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

#include <cxl/pci.h>
#include <cxl/cxl.h>
#include <linux/pci.h>

#include "net_driver.h"
#include "efx_cxl.h"

#define EFX_CTPIO_BUFFER_SIZE	SZ_256M

int efx_cxl_init(struct efx_probe_data *probe_data)
{
	struct cxl_dpa_info sfc_dpa_info = { 0 };
	struct efx_nic *efx = &probe_data->efx;
	struct pci_dev *pci_dev = efx->pci_dev;
	DECLARE_BITMAP(expected, CXL_MAX_CAPS);
	DECLARE_BITMAP(found, CXL_MAX_CAPS);
	resource_size_t max_size;
	struct mds_info sfc_mds_info;
	struct efx_cxl *cxl;

	u16 dvsec;
	int rc;

	probe_data->cxl_pio_initialised = false;

	dvsec = pci_find_dvsec_capability(pci_dev, PCI_VENDOR_ID_CXL,
					  CXL_DVSEC_PCIE_DEVICE);
	if (!dvsec)
		return 0;

	pci_dbg(pci_dev, "CXL_DVSEC_PCIE_DEVICE capability found\n");

	cxl = kzalloc(sizeof(*cxl), GFP_KERNEL);
	if (!cxl)
		return -ENOMEM;

	cxl->cxlmds = cxl_memdev_state_create(&pci_dev->dev, pci_dev->dev.id,
					      dvsec, CXL_DEVTYPE_DEVMEM);

	if (IS_ERR(cxl->cxlmds)) {
		kfree(cxl);
		return PTR_ERR(cxl->cxlmds);
	}

	bitmap_clear(expected, 0, CXL_MAX_CAPS);
	set_bit(CXL_DEV_CAP_HDM, expected);
	set_bit(CXL_DEV_CAP_HDM, expected);
	set_bit(CXL_DEV_CAP_RAS, expected);

	rc = cxl_pci_accel_setup_regs(pci_dev, cxl->cxlmds, found);
	if (rc) {
		pci_err(pci_dev, "CXL accel setup regs failed");
		goto err_regs;
	}

	/*
	 * Checking mandatory caps are there as, at least, a subset of those
	 * found.
	 */
	if (!bitmap_subset(expected, found, CXL_MAX_CAPS)) {
		pci_err(pci_dev,
			"CXL device capabilities found(%pb) not as expected(%pb)",
			found, expected);
		rc = -EIO;
		goto err_regs;
	}

	/* We do not have the register about media status. Hardware design
	 * implies it is ready.
	 */
	cxl_set_media_ready(cxl->cxlmds);

	sfc_mds_info.total_bytes = EFX_CTPIO_BUFFER_SIZE;
	sfc_mds_info.volatile_only_bytes = EFX_CTPIO_BUFFER_SIZE;
	sfc_mds_info.persistent_only_bytes = 0;

	cxl_dev_state_setup(cxl->cxlmds, &sfc_mds_info);

	rc = cxl_mem_dpa_fetch(cxl->cxlmds, &sfc_dpa_info);
	if (rc)
		goto err_regs;

	rc = cxl_dpa_setup(cxl->cxlmds, &sfc_dpa_info);
	if (rc)
		goto err_regs;

	cxl->cxlmd = devm_cxl_add_memdev(&pci_dev->dev, cxl->cxlmds);

	if (IS_ERR(cxl->cxlmd)) {
		pci_err(pci_dev, "CXL accel memdev creation failed");
		rc = PTR_ERR(cxl->cxlmd);
		goto err_regs;
	}

	cxl->cxlrd = cxl_get_hpa_freespace(cxl->cxlmd, 1,
					   CXL_DECODER_F_RAM | CXL_DECODER_F_TYPE2,
					   &max_size);

	if (IS_ERR(cxl->cxlrd)) {
		pci_err(pci_dev, "cxl_get_hpa_freespace failed\n");
		rc = PTR_ERR(cxl->cxlrd);
		goto err_regs;
	}

	if (max_size < EFX_CTPIO_BUFFER_SIZE) {
		pci_err(pci_dev, "%s: not enough free HPA space %pap < %u\n",
			__func__, &max_size, EFX_CTPIO_BUFFER_SIZE);
		rc = -ENOSPC;
		cxl_put_root_decoder(cxl->cxlrd);
		goto err_regs;
	}

	cxl->cxled = cxl_request_dpa(cxl->cxlmd, true, EFX_CTPIO_BUFFER_SIZE);
	if (IS_ERR(cxl->cxled)) {
		pci_err(pci_dev, "CXL accel request DPA failed");
		rc = PTR_ERR(cxl->cxled);
		cxl_put_root_decoder(cxl->cxlrd);
		goto err_regs;
	}

	probe_data->cxl = cxl;

	return 0;

err_regs:
	kfree(probe_data->cxl);
	return rc;

}

void efx_cxl_exit(struct efx_probe_data *probe_data)
{
	if (probe_data->cxl) {
		cxl_dpa_free(probe_data->cxl->cxled);
		cxl_put_root_decoder(probe_data->cxl->cxlrd);
		kfree(probe_data->cxl);
	}
}

MODULE_IMPORT_NS("CXL");
