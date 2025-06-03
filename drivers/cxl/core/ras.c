// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/pci.h>
#include <linux/aer.h>
#include <cxl/event.h>
#include <cxlmem.h>
#include <cxlpci.h>
#include "trace.h"

static void cxl_cper_trace_corr_port_prot_err(struct pci_dev *pdev,
					      struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.cor_status & ~ras_cap.cor_mask;

	trace_cxl_port_aer_correctable_error(&pdev->dev, status);
}

static void cxl_cper_trace_uncorr_port_prot_err(struct pci_dev *pdev,
						struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.uncor_status & ~ras_cap.uncor_mask;
	u32 fe;

	if (hweight32(status) > 1)
		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   ras_cap.cap_control));
	else
		fe = status;

	trace_cxl_port_aer_uncorrectable_error(&pdev->dev, status, fe,
					       ras_cap.header_log);
}

static void cxl_cper_trace_corr_prot_err(struct pci_dev *pdev,
				  struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.cor_status & ~ras_cap.cor_mask;
	struct cxl_dev_state *cxlds;

	cxlds = pci_get_drvdata(pdev);
	if (!cxlds)
		return;

	trace_cxl_aer_correctable_error(cxlds->cxlmd, status);
}

static void cxl_cper_trace_uncorr_prot_err(struct pci_dev *pdev,
				    struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.uncor_status & ~ras_cap.uncor_mask;
	struct cxl_dev_state *cxlds;
	u32 fe;

	cxlds = pci_get_drvdata(pdev);
	if (!cxlds)
		return;

	if (hweight32(status) > 1)
		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   ras_cap.cap_control));
	else
		fe = status;

	trace_cxl_aer_uncorrectable_error(cxlds->cxlmd, status, fe,
					  ras_cap.header_log);
}

static void cxl_cper_handle_prot_err(struct cxl_cper_prot_err_work_data *data)
{
	unsigned int devfn = PCI_DEVFN(data->prot_err.agent_addr.device,
				       data->prot_err.agent_addr.function);
	struct pci_dev *pdev __free(pci_dev_put) =
		pci_get_domain_bus_and_slot(data->prot_err.agent_addr.segment,
					    data->prot_err.agent_addr.bus,
					    devfn);
	int port_type;

	if (!pdev)
		return;

	guard(device)(&pdev->dev);

	port_type = pci_pcie_type(pdev);
	if (port_type == PCI_EXP_TYPE_ROOT_PORT ||
	    port_type == PCI_EXP_TYPE_DOWNSTREAM ||
	    port_type == PCI_EXP_TYPE_UPSTREAM) {
		if (data->severity == AER_CORRECTABLE)
			cxl_cper_trace_corr_port_prot_err(pdev, data->ras_cap);
		else
			cxl_cper_trace_uncorr_port_prot_err(pdev, data->ras_cap);

		return;
	}

	if (data->severity == AER_CORRECTABLE)
		cxl_cper_trace_corr_prot_err(pdev, data->ras_cap);
	else
		cxl_cper_trace_uncorr_prot_err(pdev, data->ras_cap);
}

static void cxl_cper_prot_err_work_fn(struct work_struct *work)
{
	struct cxl_cper_prot_err_work_data wd;

	while (cxl_cper_prot_err_kfifo_get(&wd))
		cxl_cper_handle_prot_err(&wd);
}
static DECLARE_WORK(cxl_cper_prot_err_work, cxl_cper_prot_err_work_fn);

#ifdef CONFIG_PCIEAER_CXL

static void cxl_do_recovery(struct pci_dev *pdev)
{
}

static int cxl_rch_handle_error_iter(struct pci_dev *pdev, void *data)
{
	struct cxl_prot_error_info *err_info = data;
	struct pci_dev *pdev_ref __free(pci_dev_put) = pci_dev_get(pdev);
	struct cxl_dev_state *cxlds;

	/*
	 * The capability, status, and control fields in Device 0,
	 * Function 0 DVSEC control the CXL functionality of the
	 * entire device (CXL 3.0, 8.1.3).
	 */
	if (pdev->devfn != PCI_DEVFN(0, 0))
		return 0;

	/*
	 * CXL Memory Devices must have the 502h class code set (CXL
	 * 3.0, 8.1.12.1).
	 */
	if ((pdev->class >> 8) != PCI_CLASS_MEMORY_CXL)
		return 0;

	if (!is_cxl_memdev(&pdev->dev) || !pdev->dev.driver)
		return 0;

	cxlds = pci_get_drvdata(pdev);
	struct device *dev __free(put_device) = get_device(&cxlds->cxlmd->dev);

	if (err_info->severity == AER_CORRECTABLE)
		cxl_cor_error_detected(pdev);
	else
		cxl_do_recovery(pdev);

	return 1;
}

static struct pci_dev *sbdf_to_pci(struct cxl_prot_error_info *err_info)
{
	unsigned int devfn = PCI_DEVFN(err_info->device,
				       err_info->function);
	struct pci_dev *pdev __free(pci_dev_put) =
		pci_get_domain_bus_and_slot(err_info->segment,
					    err_info->bus,
					    devfn);
	return pdev;
}

static void cxl_handle_prot_error(struct cxl_prot_error_info *err_info)
{
	struct pci_dev *pdev __free(pci_dev_put) = pci_dev_get(sbdf_to_pci(err_info));

	if (!pdev) {
		pr_err("Failed to find the CXL device\n");
		return;
	}

	/*
	 * Internal errors of an RCEC indicate an AER error in an
	 * RCH's downstream port. Check and handle them in the CXL.mem
	 * device driver.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_EC)
		return pcie_walk_rcec(pdev, cxl_rch_handle_error_iter, err_info);

	if (err_info->severity == AER_CORRECTABLE) {
		int aer = pdev->aer_cap;
		struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
		struct device *dev __free(put_device) = get_device(&cxlds->cxlmd->dev);

		if (aer)
			pci_clear_and_set_config_dword(pdev,
						       aer + PCI_ERR_COR_STATUS,
						       0, PCI_ERR_COR_INTERNAL);

		cxl_cor_error_detected(pdev);

		pcie_clear_device_status(pdev);
	} else {
		cxl_do_recovery(pdev);
	}
}

static void cxl_prot_err_work_fn(struct work_struct *work)
{
	struct cxl_prot_err_work_data wd;

	while (cxl_prot_err_kfifo_get(&wd)) {
		struct cxl_prot_error_info *err_info = &wd.err_info;

		cxl_handle_prot_error(err_info);
	}
}

#else
static void cxl_prot_err_work_fn(struct work_struct *work) { }
#endif /* CONFIG_PCIEAER_CXL */

static struct work_struct cxl_prot_err_work;
static DECLARE_WORK(cxl_prot_err_work, cxl_prot_err_work_fn);

int cxl_ras_init(void)
{
	int rc;

	rc = cxl_cper_register_prot_err_work(&cxl_cper_prot_err_work);
	if (rc)
		pr_err("Failed to register CPER AER kfifo (%x)", rc);

	rc = cxl_register_prot_err_work(&cxl_prot_err_work);
	if (rc) {
		pr_err("Failed to register native AER kfifo (%x)", rc);
		return rc;
	}

	return 0;
}

void cxl_ras_exit(void)
{
	cxl_cper_unregister_prot_err_work(&cxl_cper_prot_err_work);
	cancel_work_sync(&cxl_cper_prot_err_work);

	cxl_unregister_prot_err_work();
	cancel_work_sync(&cxl_prot_err_work);
}
