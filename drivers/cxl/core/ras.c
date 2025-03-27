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

	trace_cxl_aer_correctable_error(&pdev->dev, &pdev->dev, 0, status);
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

	trace_cxl_aer_uncorrectable_error(&pdev->dev, &pdev->dev, 0,
					  status, fe, ras_cap.header_log);
}

static void cxl_cper_trace_corr_prot_err(struct pci_dev *pdev,
				  struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.cor_status & ~ras_cap.cor_mask;
	struct cxl_dev_state *cxlds;

	cxlds = pci_get_drvdata(pdev);
	if (!cxlds)
		return;

	trace_cxl_aer_correctable_error(&cxlds->cxlmd->dev, &pdev->dev,
					cxlds->serial, status);
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

	trace_cxl_aer_uncorrectable_error(&cxlds->cxlmd->dev, &pdev->dev,
					  cxlds->serial, status,
					  fe, ras_cap.header_log);
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

int cxl_create_prot_err_info(struct pci_dev *_pdev, int severity,
			     struct cxl_prot_error_info *err_info)
{
	struct pci_dev *pdev __free(pci_dev_put) = pci_dev_get(_pdev);
	struct cxl_dev_state *cxlds;

	if (!pdev || !err_info) {
		pr_warn_once("Error: parameter is NULL");
		return -ENODEV;
	}

	if ((pci_pcie_type(pdev) != PCI_EXP_TYPE_ENDPOINT) &&
	    (pci_pcie_type(pdev) != PCI_EXP_TYPE_RC_END)) {
		pci_warn_once(pdev, "Error: Unsupported device type (%X)", pci_pcie_type(pdev));
		return -ENODEV;
	}

	cxlds = pci_get_drvdata(pdev);
	struct device *dev __free(put_device) = get_device(&cxlds->cxlmd->dev);

	if (!dev)
		return -ENODEV;

	*err_info = (struct cxl_prot_error_info){ 0 };
	err_info->ras_base = cxlds->regs.ras;
	err_info->severity = severity;
	err_info->pdev = pdev;
	err_info->dev = dev;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_create_prot_err_info, "CXL");


static pci_ers_result_t merge_result(enum pci_ers_result orig,
				     enum pci_ers_result new)
{
	if (new == PCI_ERS_RESULT_PANIC)
		return PCI_ERS_RESULT_PANIC;

	if (new == PCI_ERS_RESULT_NO_AER_DRIVER)
		return PCI_ERS_RESULT_NO_AER_DRIVER;

	if (new == PCI_ERS_RESULT_NONE)
		return orig;

	switch (orig) {
	case PCI_ERS_RESULT_CAN_RECOVER:
	case PCI_ERS_RESULT_RECOVERED:
		orig = new;
		break;
	case PCI_ERS_RESULT_DISCONNECT:
		if (new == PCI_ERS_RESULT_NEED_RESET)
			orig = PCI_ERS_RESULT_NEED_RESET;
		break;
	default:
		break;
	}

	return orig;
}

static void cxl_walk_bridge(struct pci_dev *bridge,
			    int (*cb)(struct pci_dev *, void *),
			    void *userdata)
{
	if (cb(bridge, userdata))
		return;

	if (bridge->subordinate)
		pci_walk_bus(bridge->subordinate, cb, userdata);
}


static int cxl_report_error_detected(struct pci_dev *pdev, void *data)
{
	struct cxl_driver *pdrv;
	pci_ers_result_t vote, *result = data;
	struct cxl_prot_error_info err_info = { 0 };
	const struct cxl_error_handlers *cxl_err_handler;

	if (cxl_create_prot_err_info(pdev, AER_FATAL, &err_info))
		return 0;

	struct device *dev __free(put_device) = get_device(err_info.dev);
	if (!dev)
		return 0;

	pdrv = to_cxl_drv(dev->driver);
	if (!pdrv || !pdrv->err_handler ||
	    !pdrv->err_handler->error_detected)
		return 0;

	cxl_err_handler = pdrv->err_handler;
	vote = cxl_err_handler->error_detected(dev, &err_info);

	*result = merge_result(*result, vote);

	return 0;
}

static void cxl_do_recovery(struct pci_dev *pdev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(pdev->bus);
	pci_ers_result_t status = PCI_ERS_RESULT_CAN_RECOVER;

	cxl_walk_bridge(pdev, cxl_report_error_detected, &status);
	if (status == PCI_ERS_RESULT_PANIC)
		panic("CXL cachemem error.");

	/*
	 * If we have native control of AER, clear error status in the device
	 * that detected the error.  If the platform retained control of AER,
	 * it is responsible for clearing this status.  In that case, the
	 * signaling device may not even be visible to the OS.
	 */
	if (host->native_aer) {
		pcie_clear_device_status(pdev);
		pci_aer_clear_nonfatal_status(pdev);
		pci_aer_clear_fatal_status(pdev);
	}

	pci_info(pdev, "CXL uncorrectable error.\n");
}

static int cxl_rch_handle_error_iter(struct pci_dev *pdev, void *data)
{
	struct cxl_prot_error_info *err_info = data;
	const struct cxl_error_handlers *err_handler;
	struct device *dev = err_info->dev;
	struct cxl_driver *pdrv;

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

	if (!is_cxl_memdev(dev) || !dev->driver)
		return 0;

	pdrv = to_cxl_drv(dev->driver);
	if (!pdrv || !pdrv->err_handler)
		return 0;

	err_handler = pdrv->err_handler;
	if (err_info->severity == AER_CORRECTABLE) {
		if (err_handler->cor_error_detected)
			err_handler->cor_error_detected(dev, err_info);
	} else if (err_handler->error_detected) {
		cxl_do_recovery(pdev);
	}

	return 0;
}

static void cxl_handle_prot_error(struct pci_dev *pdev, struct cxl_prot_error_info *err_info)
{
	if (!pdev || !err_info)
		return;

	/*
	 * Internal errors of an RCEC indicate an AER error in an
	 * RCH's downstream port. Check and handle them in the CXL.mem
	 * device driver.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_EC)
		return pcie_walk_rcec(pdev, cxl_rch_handle_error_iter, err_info);

	if (err_info->severity == AER_CORRECTABLE) {
		struct device *dev __free(put_device) = get_device(err_info->dev);
		struct cxl_driver *pdrv;
		int aer = pdev->aer_cap;

		if (!dev || !dev->driver)
			return;

		if (aer) {
			int ras_status;

			pci_read_config_dword(pdev, aer + PCI_ERR_COR_STATUS, &ras_status);
			pci_write_config_dword(pdev, aer + PCI_ERR_COR_STATUS,
					       ras_status);
		}

		pdrv = to_cxl_drv(dev->driver);
		if (!pdrv || !pdrv->err_handler ||
		    !pdrv->err_handler->cor_error_detected)
			return;

		pdrv->err_handler->cor_error_detected(dev, err_info);
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
		struct device *dev __free(put_device) = get_device(err_info->dev);
		struct pci_dev *pdev __free(pci_dev_put) = pci_dev_get(err_info->pdev);

		if (!dev || !pdev)
			continue;

		cxl_handle_prot_error(pdev, err_info);
	}
}

static DECLARE_WORK(cxl_prot_err_work, cxl_prot_err_work_fn);

int cxl_ras_init(void)
{
	int rc;

	rc = cxl_cper_register_prot_err_work(&cxl_cper_prot_err_work);
	if (rc) {
		pr_err("Failed to register CPER kfifo with AER driver");
		return rc;
	}

	rc = cxl_register_prot_err_work(&cxl_prot_err_work, cxl_create_prot_err_info);
	if (rc) {
		pr_err("Failed to register kfifo with AER driver");
		return rc;
	}

	return rc;
}

void cxl_ras_exit(void)
{
	cxl_cper_unregister_prot_err_work(&cxl_cper_prot_err_work);
	cancel_work_sync(&cxl_cper_prot_err_work);

	cxl_unregister_prot_err_work();
	cancel_work_sync(&cxl_prot_err_work);
}
