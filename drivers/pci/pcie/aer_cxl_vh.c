// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/types.h>
#include <linux/kfifo.h>
#include <linux/aer.h>
#include "../pci.h"
#include "portdrv.h"

#define CXL_ERROR_SOURCES_MAX          128

struct cxl_proto_err_kfifo {
	struct work_struct *work;
	struct rw_semaphore rwsema;
	DECLARE_KFIFO(fifo, struct cxl_proto_err_work_data,
		      CXL_ERROR_SOURCES_MAX);
};

static struct cxl_proto_err_kfifo cxl_proto_err_kfifo = {
	.rwsema = __RWSEM_INITIALIZER(cxl_proto_err_kfifo.rwsema)
};

bool is_aer_internal_error(struct aer_err_info *info)
{
	if (info->severity == AER_CORRECTABLE)
		return info->status & PCI_ERR_COR_INTERNAL;

	return info->status & PCI_ERR_UNC_INTN;
}

bool is_cxl_error(struct pci_dev *pdev, struct aer_err_info *info)
{
	if (!info || !info->is_cxl)
		return false;

	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ENDPOINT)
		return false;

	return is_aer_internal_error(info);
}

void cxl_forward_error(struct pci_dev *pdev, struct aer_err_info *info)
{
	struct cxl_proto_err_work_data wd = (struct cxl_proto_err_work_data) {
		.severity = info->severity,
		.pdev = pdev
	};

	guard(rwsem_read)(&cxl_proto_err_kfifo.rwsema);

	if (!cxl_proto_err_kfifo.work) {
		dev_err_ratelimited(&pdev->dev, "AER-CXL kfifo reader not registered");
		return;
	}

	/* The reference is held as long as the pdev is live in the kfifo */
	pci_dev_get(pdev);

	if (!kfifo_put(&cxl_proto_err_kfifo.fifo, wd)) {
		dev_err_ratelimited(&pdev->dev, "AER-CXL kfifo add failed");
		pci_dev_put(pdev);
		return;
	}

	schedule_work(cxl_proto_err_kfifo.work);
}

void cxl_register_proto_err_work(struct work_struct *work)
{
	guard(rwsem_write)(&cxl_proto_err_kfifo.rwsema);
	cxl_proto_err_kfifo.work = work;
}
EXPORT_SYMBOL_NS_GPL(cxl_register_proto_err_work, "CXL");

void cxl_unregister_proto_err_work(void)
{
	struct work_struct *work;

	down_write(&cxl_proto_err_kfifo.rwsema);
	work = cxl_proto_err_kfifo.work;
	cxl_proto_err_kfifo.work = NULL;
	up_write(&cxl_proto_err_kfifo.rwsema);

	if (work)
		cancel_work_sync(work);
}
EXPORT_SYMBOL_NS_GPL(cxl_unregister_proto_err_work, "CXL");

int cxl_proto_err_kfifo_get(struct cxl_proto_err_work_data *wd)
{
	guard(rwsem_read)(&cxl_proto_err_kfifo.rwsema);
	return kfifo_get(&cxl_proto_err_kfifo.fifo, wd);
}
EXPORT_SYMBOL_NS_GPL(cxl_proto_err_kfifo_get, "CXL");
