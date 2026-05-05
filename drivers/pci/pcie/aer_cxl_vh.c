// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 AMD Corporation. All rights reserved. */

#include <linux/aer.h>
#include <linux/cleanup.h>
#include <linux/init.h>
#include <linux/kfifo.h>
#include <linux/rwsem.h>
#include <linux/workqueue.h>
#include "../pci.h"
#include "portdrv.h"

#define CXL_ERROR_SOURCES_MAX          128

struct cxl_proto_err_kfifo {
	struct work_struct *work;
	struct rw_semaphore rwsem;
	spinlock_t fifo_lock;
	DECLARE_KFIFO(fifo, struct cxl_proto_err_work_data,
		      CXL_ERROR_SOURCES_MAX);
};

static struct cxl_proto_err_kfifo cxl_proto_err_kfifo = {
	.rwsem = __RWSEM_INITIALIZER(cxl_proto_err_kfifo.rwsem),
	.fifo_lock = __SPIN_LOCK_UNLOCKED(cxl_proto_err_kfifo.fifo_lock),
};

static int __init cxl_proto_err_kfifo_init(void)
{
	INIT_KFIFO(cxl_proto_err_kfifo.fifo);
	return 0;
}
subsys_initcall(cxl_proto_err_kfifo_init);

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
	struct cxl_proto_err_work_data wd = {
		.severity = info->severity,
		.pdev = pdev,
	};

	if (info->severity == AER_CORRECTABLE)
		pci_write_config_dword(pdev, pdev->aer_cap + PCI_ERR_COR_STATUS,
				       info->status);

	guard(rwsem_read)(&cxl_proto_err_kfifo.rwsem);

	if (!cxl_proto_err_kfifo.work) {
		dev_err_ratelimited(&pdev->dev, "AER-CXL kfifo reader not registered\n");
		return;
	}

	/*
	 * Reference discipline: the AER caller (handle_error_source())
	 * holds a ref on @pdev for the duration of this call and releases
	 * it on return. Take a fresh ref here so the pdev stays live while
	 * queued in the kfifo; the consumer (for_each_cxl_proto_err())
	 * drops that ref after handling. On enqueue failure below, drop
	 * the ref we just took to avoid a leak.
	 */
	pci_dev_get(pdev);

	/* Serialize concurrent kfifo writers: multiple AER threaded IRQs */
	if (!kfifo_in_spinlocked(&cxl_proto_err_kfifo.fifo, &wd, 1,
				 &cxl_proto_err_kfifo.fifo_lock)) {
		dev_err_ratelimited(&pdev->dev, "AER-CXL kfifo add failed\n");
		pci_dev_put(pdev);
		return;
	}

	schedule_work(cxl_proto_err_kfifo.work);
}

void cxl_register_proto_err_work(struct work_struct *work)
{
	guard(rwsem_write)(&cxl_proto_err_kfifo.rwsem);
	WARN_ONCE(cxl_proto_err_kfifo.work,
		  "AER-CXL kfifo consumer already registered\n");
	cxl_proto_err_kfifo.work = work;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_register_proto_err_work, "cxl_core");

static struct work_struct *cancel_cxl_proto_err(void)
{
	struct work_struct *work;
	struct cxl_proto_err_work_data wd;

	guard(rwsem_write)(&cxl_proto_err_kfifo.rwsem);
	work = cxl_proto_err_kfifo.work;
	cxl_proto_err_kfifo.work = NULL;
	while (kfifo_get(&cxl_proto_err_kfifo.fifo, &wd)) {
		dev_err_ratelimited(&wd.pdev->dev,
				    "AER-CXL error report canceled\n");
		pci_dev_put(wd.pdev);
	}
	return work;
}

void cxl_unregister_proto_err_work(void)
{
	struct work_struct *work = cancel_cxl_proto_err();

	if (work)
		cancel_work_sync(work);
}
EXPORT_SYMBOL_FOR_MODULES(cxl_unregister_proto_err_work, "cxl_core");

int for_each_cxl_proto_err(struct cxl_proto_err_work_data *wd,
			   cxl_proto_err_fn_t fn)
{
	int rc;

	guard(rwsem_read)(&cxl_proto_err_kfifo.rwsem);
	while (kfifo_get(&cxl_proto_err_kfifo.fifo, wd)) {
		rc = fn(wd);
		pci_dev_put(wd->pdev);
		if (rc)
			return rc;
	}

	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(for_each_cxl_proto_err, "cxl_core");
