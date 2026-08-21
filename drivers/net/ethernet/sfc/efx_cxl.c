// SPDX-License-Identifier: GPL-2.0-only
/****************************************************************************
 *
 * Driver for AMD network controllers and boards
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#include <linux/pci.h>

#include <cxl/cxl.h>
#include <cxl/pci.h>
#include "net_driver.h"
#include "efx_cxl.h"

#define EFX_CTPIO_BUFFER_SIZE	SZ_256M
#define EFX_CTPIO_BUFFER_PER_PF_SIZE	SZ_8M

#define X4_PF_DEVICE_ID 0x0c03

static struct pci_dev *get_pf0_pci_device(struct pci_dev *pfx)
{
	struct pci_dev *pf0_pci_dev;

	while ((pf0_pci_dev = pci_get_device(PCI_VENDOR_ID_SOLARFLARE,
					     X4_PF_DEVICE_ID, pf0_pci_dev))
					     != NULL) {
		/* With multiple X4 installed check against pci_slot as well. */
		if (pf0_pci_dev->slot == pfx->slot &&
		    PCI_FUNC(pf0_pci_dev->devfn) == 0)
			break;
	}
	return pf0_pci_dev;
}

static int cxl_map(struct efx_probe_data *probe_data, struct efx_cxl *cxl,
		   u64 devfn, struct range cxl_pio_range)
{
	struct efx_nic *efx = &probe_data->efx;
	struct pci_dev *pci_dev = efx->pci_dev;
	u64 cxl_pio_pf_start;

	cxl_pio_pf_start = cxl_pio_range.start + devfn *
			   EFX_CTPIO_BUFFER_PER_PF_SIZE;

	cxl->ctpio_cxl = ioremap_wc(cxl_pio_pf_start,
				    EFX_CTPIO_BUFFER_PER_PF_SIZE);
	if (!cxl->ctpio_cxl) {
		pci_err(pci_dev, "CXL ioremap region (%pra) failed\n",
			&cxl_pio_range);
		return -ENOMEM;
	}

	probe_data->cxl = cxl;
	probe_data->cxl_pio_initialised = true;

	return 0;
}

int efx_cxl_init(struct efx_probe_data *probe_data)
{
	struct efx_nic *efx = &probe_data->efx;
	struct pci_dev *pci_dev = efx->pci_dev;
	struct range cxl_pio_range;
	struct efx_cxl *cxl;
	u8 devfn;
	u16 dvsec;
	int rc;

	if (efx->type->is_vf)
		return 0;

	/* are we PF0? */
	devfn = PCI_FUNC(pci_dev->devfn);
	if (devfn != 0) {
		struct pci_dev *pf0_pci_dev;
		struct cxl_memdev *cxlmd;

		pf0_pci_dev = get_pf0_pci_device(pci_dev);

		/* This should not happen! */
		if (!pf0_pci_dev)
			return 0;

		/* Is the PF0 device configured with and using CXL? */
		if (!pcie_is_cxl(pf0_pci_dev))
			return 0;

		cxlmd = cxl_get_pf0_memdev(&pf0_pci_dev->dev, &pci_dev->dev,
					   devfn, &cxl_pio_range);

		if (IS_ERR(cxlmd))
			return -EPROBE_DEFER;

		cxl = kzalloc_obj(struct cxl);
		if (!cxl)
			return -ENOMEM;

		cxl->cxlmd = cxlmd;

		if (!cxl_map(probe_data, cxl, (u64)devfn, cxl_pio_range)) {
			kfree(cxl);
			return -ENOMEM;
		}
		return 0;
	}

	/* Is the device configured with and using CXL? */
	if (!pcie_is_cxl(pci_dev))
		return 0;

	dvsec = pci_find_dvsec_capability(pci_dev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_DEVICE);
	if (!dvsec) {
		pci_info(pci_dev, "CXL_DVSEC_PCIE_DEVICE capability not found\n");
		return 0;
	}

	pci_dbg(pci_dev, "CXL_DVSEC_PCIE_DEVICE capability found\n");

	/* Create a cxl_dev_state embedded in the cxl struct using cxl core api
	 * specifying no mbox available.
	 */
	cxl = devm_cxl_dev_state_create(&pci_dev->dev, CXL_DEVTYPE_DEVMEM,
					pci_get_dsn(pci_dev), dvsec,
					struct efx_cxl, cxlds, false);

	if (!cxl)
		return -ENOMEM;

	rc = cxl_pci_setup_regs(pci_dev, CXL_REGLOC_RBI_COMPONENT,
				&cxl->cxlds.reg_map);
	if (rc) {
		pci_err(pci_dev, "No component registers\n");
		return rc;
	}

	if (!cxl->cxlds.reg_map.component_map.hdm_decoder.valid) {
		pci_err(pci_dev, "Expected HDM component register not found\n");
		return -ENODEV;
	}

	if (!cxl->cxlds.reg_map.component_map.ras.valid) {
		pci_err(pci_dev, "Expected RAS component register not found\n");
		return -ENODEV;
	}

	/* Set media ready explicitly as there are neither mailbox for checking
	 * this state nor the CXL register involved, both not mandatory for
	 * type2.
	 */
	cxl->cxlds.media_ready = true;

	if (cxl_set_capacity(&cxl->cxlds, EFX_CTPIO_BUFFER_SIZE)) {
		pci_err(pci_dev, "dpa capacity setup failed\n");
		return -ENODEV;
	}

	cxl->cxlmd = devm_cxl_probe_mem(&cxl->cxlds, &cxl_pio_range);
	if (IS_ERR(cxl->cxlmd)) {
		pci_err(pci_dev, "CXL accel memdev creation failed\n");
		return PTR_ERR(cxl->cxlmd);
	}

	if (!cxl_map(probe_data, cxl, 0, cxl_pio_range)) {
		pci_err(pci_dev, "CXL ioremap region (%pra) failed\n",
			&cxl_pio_range);
		return -ENOMEM;
	}

	return 0;
}

void efx_cxl_exit(struct efx_probe_data *probe_data)
{
	struct efx_nic *efx = &probe_data->efx;
	struct pci_dev *pci_dev = efx->pci_dev;
	u8 devfn;

	if (!probe_data->cxl)
		return;

	/* are we PF0? */
	devfn = PCI_FUNC(pci_dev->devfn);
	if (devfn != 0) {
		struct pci_dev *pf0_pci_dev;

		pf0_pci_dev = get_pf0_pci_device(pci_dev);

		/* This should not happen! */
		if (!pf0_pci_dev)
			return;

		cxl_put_pf0_memdev(&pf0_pci_dev->dev, &pci_dev->dev, devfn);
	}

	iounmap(probe_data->cxl->ctpio_cxl);
}

MODULE_IMPORT_NS("CXL");
