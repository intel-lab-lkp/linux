// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/pci.h>
#include <linux/bitfield.h>
#include <linux/kfifo.h>
#include "../pci.h"

#define CXL_ERROR_SOURCES_MAX          128

struct cxl_proto_err_kfifo {
	struct work_struct *work;
	struct rw_semaphore rw_sema;
	DECLARE_KFIFO(fifo, struct cxl_proto_err_work_data,
		      CXL_ERROR_SOURCES_MAX);
};

static struct cxl_proto_err_kfifo cxl_proto_err_kfifo = {
	.rw_sema = __RWSEM_INITIALIZER(cxl_proto_err_kfifo.rw_sema)
};

/**
 * pci_aer_unmask_internal_errors - unmask internal errors
 * @dev: pointer to the pci_dev data structure
 *
 * Unmask internal errors in the Uncorrectable and Correctable Error
 * Mask registers.
 *
 * Note: AER must be enabled and supported by the device which must be
 * checked in advance, e.g. with pcie_aer_is_native().
 */
void pci_aer_unmask_internal_errors(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	u32 mask;

	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask);
	mask &= ~PCI_ERR_UNC_INTN;
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, mask);

	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask);
	mask &= ~PCI_ERR_COR_INTERNAL;
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, mask);
}
EXPORT_SYMBOL_NS_GPL(pci_aer_unmask_internal_errors, "CXL");

bool cxl_error_is_native(struct pci_dev *dev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

	return (pcie_ports_native || host->native_aer);
}
EXPORT_SYMBOL_NS_GPL(cxl_error_is_native, "CXL");

bool is_internal_error(struct aer_err_info *info)
{
	if (info->severity == AER_CORRECTABLE)
		return info->status & PCI_ERR_COR_INTERNAL;

	return info->status & PCI_ERR_UNC_INTN;
}
EXPORT_SYMBOL_NS_GPL(is_internal_error, "CXL");

bool is_cxl_error(struct pci_dev *pdev, struct aer_err_info *info)
{
	if (!info || !info->is_cxl)
		return false;

	if ((pci_pcie_type(pdev) != PCI_EXP_TYPE_ENDPOINT) &&
	    (pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT) &&
	    (pci_pcie_type(pdev) != PCI_EXP_TYPE_UPSTREAM) &&
	    (pci_pcie_type(pdev) != PCI_EXP_TYPE_DOWNSTREAM))
		return false;

	return is_internal_error(info);
}
EXPORT_SYMBOL_NS_GPL(is_cxl_error, "CXL");

void cxl_forward_error(struct pci_dev *pdev, struct aer_err_info *info)
{
	struct cxl_proto_err_work_data wd = (struct cxl_proto_err_work_data) {
		.severity = info->severity,
		.pdev = pdev
	};

	guard(rwsem_write)(&cxl_proto_err_kfifo.rw_sema);

	if (!cxl_proto_err_kfifo.work) {
		dev_warn_once(&pdev->dev, "CXL driver is not registered for kfifo");
		return;
	}

	if (!kfifo_put(&cxl_proto_err_kfifo.fifo, wd)) {
		dev_err_ratelimited(&pdev->dev, "CXL kfifo overflow\n");
		return;
	}

	schedule_work(cxl_proto_err_kfifo.work);
}
EXPORT_SYMBOL_NS_GPL(cxl_forward_error, "CXL");

void cxl_register_proto_err_work(struct work_struct *work)
{
	guard(rwsem_write)(&cxl_proto_err_kfifo.rw_sema);
	cxl_proto_err_kfifo.work = work;
}
EXPORT_SYMBOL_NS_GPL(cxl_register_proto_err_work, "CXL");

void cxl_unregister_proto_err_work(void)
{
	guard(rwsem_write)(&cxl_proto_err_kfifo.rw_sema);
	cxl_proto_err_kfifo.work = NULL;
}
EXPORT_SYMBOL_NS_GPL(cxl_unregister_proto_err_work, "CXL");

int cxl_proto_err_kfifo_get(struct cxl_proto_err_work_data *wd)
{
	guard(rwsem_read)(&cxl_proto_err_kfifo.rw_sema);
	return kfifo_get(&cxl_proto_err_kfifo.fifo, wd);
}
EXPORT_SYMBOL_NS_GPL(cxl_proto_err_kfifo_get, "CXL");
