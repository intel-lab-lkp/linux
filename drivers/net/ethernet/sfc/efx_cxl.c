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

#include <cxl/cxl.h>
#include <cxl/pci.h>
#include <linux/pci.h>

#include "net_driver.h"
#include "efx_cxl.h"

#define EFX_CTPIO_BUFFER_SIZE	SZ_256M

int efx_cxl_init(struct efx_probe_data *probe_data)
{
	struct efx_nic *efx = &probe_data->efx;
	DECLARE_BITMAP(expected, CXL_MAX_CAPS);
	DECLARE_BITMAP(found, CXL_MAX_CAPS);
	resource_size_t max_size;
	struct pci_dev *pci_dev;
	struct efx_cxl *cxl;
	struct resource res;
	struct range range;
	u16 dvsec;
	int rc;

	pci_dev = efx->pci_dev;
	probe_data->cxl_pio_initialised = false;

	dvsec = pci_find_dvsec_capability(pci_dev, PCI_VENDOR_ID_CXL,
					  CXL_DVSEC_PCIE_DEVICE);
	if (!dvsec)
		return 0;

	pci_dbg(pci_dev, "CXL_DVSEC_PCIE_DEVICE capability found\n");

	cxl = kzalloc(sizeof(*cxl), GFP_KERNEL);
	if (!cxl)
		return -ENOMEM;

	cxl->cxlds = cxl_accel_state_create(&pci_dev->dev);
	if (IS_ERR(cxl->cxlds)) {
		pci_err(pci_dev, "CXL accel device state failed");
		rc = -ENOMEM;
		goto err_state;
	}

	cxl_set_dvsec(cxl->cxlds, dvsec);
	cxl_set_serial(cxl->cxlds, pci_dev->dev.id);

	res = DEFINE_RES_MEM(0, EFX_CTPIO_BUFFER_SIZE);
	if (cxl_set_resource(cxl->cxlds, res, CXL_RES_DPA)) {
		pci_err(pci_dev, "cxl_set_resource DPA failed\n");
		rc = -EINVAL;
		goto err_resource_set;
	}

	res = DEFINE_RES_MEM_NAMED(0, EFX_CTPIO_BUFFER_SIZE, "ram");
	if (cxl_set_resource(cxl->cxlds, res, CXL_RES_RAM)) {
		pci_err(pci_dev, "cxl_set_resource RAM failed\n");
		rc = -EINVAL;
		goto err_resource_set;
	}

	rc = cxl_pci_accel_setup_regs(pci_dev, cxl->cxlds);
	if (rc) {
		pci_err(pci_dev, "CXL accel setup regs failed");
		goto err_resource_set;
	}

	bitmap_clear(expected, 0, CXL_MAX_CAPS);
	bitmap_set(expected, CXL_DEV_CAP_HDM, 1);
	bitmap_set(expected, CXL_DEV_CAP_RAS, 1);

	if (!cxl_pci_check_caps(cxl->cxlds, expected, found)) {
		pci_err(pci_dev,
			"CXL device capabilities found(%08lx) not as expected(%08lx)",
			*found, *expected);
		rc = -EIO;
		goto err_resource_set;
	}

	rc = cxl_request_resource(cxl->cxlds, CXL_RES_RAM);
	if (rc) {
		pci_err(pci_dev, "CXL request resource failed");
		goto err_resource_set;
	}

	/* We do not have the register about media status. Hardware design
	 * implies it is ready.
	 */
	cxl_set_media_ready(cxl->cxlds);

	cxl->cxlmd = devm_cxl_add_memdev(&pci_dev->dev, cxl->cxlds);
	if (IS_ERR(cxl->cxlmd)) {
		pci_err(pci_dev, "CXL accel memdev creation failed");
		rc = PTR_ERR(cxl->cxlmd);
		goto err_memdev;
	}

	cxl->cxlrd = cxl_get_hpa_freespace(cxl->cxlmd,
					   CXL_DECODER_F_RAM | CXL_DECODER_F_TYPE2,
					   &max_size);

	if (IS_ERR(cxl->cxlrd)) {
		pci_err(pci_dev, "cxl_get_hpa_freespace failed\n");
		rc = PTR_ERR(cxl->cxlrd);
		goto err_memdev;
	}

	if (max_size < EFX_CTPIO_BUFFER_SIZE) {
		pci_err(pci_dev, "%s: not enough free HPA space %pap < %u\n",
			__func__, &max_size, EFX_CTPIO_BUFFER_SIZE);
		rc = -ENOSPC;
		goto err_memdev;
	}

	cxl->cxled = cxl_request_dpa(cxl->cxlmd, true, EFX_CTPIO_BUFFER_SIZE,
				     EFX_CTPIO_BUFFER_SIZE);
	if (IS_ERR(cxl->cxled)) {
		pci_err(pci_dev, "CXL accel request DPA failed");
		rc = PTR_ERR(cxl->cxlrd);
		goto err_memdev;
	}

	cxl->efx_region = cxl_create_region(cxl->cxlrd, cxl->cxled, true);
	if (IS_ERR(cxl->efx_region)) {
		pci_err(pci_dev, "CXL accel create region failed");
		rc = PTR_ERR(cxl->efx_region);
		goto err_region;
	}

	rc = cxl_get_region_range(cxl->efx_region, &range);
	if (rc) {
		pci_err(pci_dev, "CXL getting regions params failed");
		goto err_region_params;
	}

	cxl->ctpio_cxl = ioremap(range.start, range.end - range.start);
	if (!cxl->ctpio_cxl) {
		pci_err(pci_dev, "CXL ioremap region (%pra) pfailed", &range);
		goto err_region_params;
	}

	probe_data->cxl = cxl;
	probe_data->cxl_pio_initialised = true;

	return 0;

err_region_params:
	cxl_accel_region_detach(cxl->cxled);
err_region:
	cxl_dpa_free(cxl->cxled);
err_memdev:
	cxl_release_resource(cxl->cxlds, CXL_RES_RAM);
err_resource_set:
	kfree(cxl->cxlds);
err_state:
	kfree(cxl);
	return rc;
}

void efx_cxl_exit(struct efx_probe_data *probe_data)
{
	if (probe_data->cxl) {
		iounmap(probe_data->cxl->ctpio_cxl);
		cxl_accel_region_detach(probe_data->cxl->cxled);
		cxl_dpa_free(probe_data->cxl->cxled);
		cxl_release_resource(probe_data->cxl->cxlds, CXL_RES_RAM);
		kfree(probe_data->cxl->cxlds);
		kfree(probe_data->cxl);
	}
}

MODULE_IMPORT_NS("CXL");
