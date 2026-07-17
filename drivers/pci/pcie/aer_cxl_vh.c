// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 AMD Corporation. All rights reserved. */

#include <linux/aer.h>
#include <linux/atomic.h>
#include <linux/cleanup.h>
#include <linux/init.h>
#include <linux/kfifo.h>
#include <linux/rwsem.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>
#include "../pci.h"
#include "portdrv.h"

#define CXL_ERROR_SOURCES_MAX          128

struct cxl_proto_err_kfifo {
	struct work_struct *work;
	void (*flush)(void);
	struct rw_semaphore rwsem;
	spinlock_t fifo_lock;
	atomic_t flush_inflight;
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

	/*
	 * RC_END (Restricted CXL Device) is not included here because RC_END
	 * reports errors on behalf of upstream RCH Downstream Port and thus
	 * requires a unique discovery detailed in CXL4.0 spec (12.2.1.1).
	 * The RCH device error discovery and RC_END forwarding flow begins
	 * in cxl_rch_handle_error().
	 */
	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ENDPOINT:
	case PCI_EXP_TYPE_ROOT_PORT:
	case PCI_EXP_TYPE_UPSTREAM:
	case PCI_EXP_TYPE_DOWNSTREAM:
		break;
	default:
		return false;
	}

	return is_aer_internal_error(info);
}

/**
 * cxl_forward_error - Forward a CXL protocol error to the CXL subsystem via kfifo
 * @pdev: PCI device that reported the AER error
 * @info: AER error info containing severity and status
 *
 * Producer side of the AER-CXL kfifo. Enqueues a CXL protocol error work
 * item and schedules the consumer workqueue. Takes a reference on @pdev
 * that the consumer releases after handling.
 *
 * Return: true if the caller must flush the kfifo before AER recovery,
 * false if no CXL error handling was initiated due to early return on
 * error.
 */
bool cxl_forward_error(struct pci_dev *pdev, struct aer_err_info *info)
{
	struct cxl_proto_err_work_data wd = {
		.severity = info->severity,
		.pdev = pdev,
	};

	guard(rwsem_read)(&cxl_proto_err_kfifo.rwsem);

	if (!cxl_proto_err_kfifo.work) {
		dev_err_ratelimited(&pdev->dev, "AER-CXL kfifo reader not registered\n");
		return false;
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
		/* Dropped; no panic - UCE unconfirmed without RAS read */
		dev_err_ratelimited(&pdev->dev, "AER-CXL kfifo add failed\n");
		pci_dev_put(pdev);
		schedule_work(cxl_proto_err_kfifo.work);
		return true;
	}

	schedule_work(cxl_proto_err_kfifo.work);
	return true;
}

void cxl_register_proto_err_work(struct work_struct *work,
				void (*flush)(void))
{
	guard(rwsem_write)(&cxl_proto_err_kfifo.rwsem);

	/*
	 * Warn on double-registration to surface driver bugs (e.g. missing
	 * cxl_unregister_proto_err_work() on module exit)
	 */
	if (WARN(cxl_proto_err_kfifo.work,
		 "AER-CXL kfifo consumer already registered\n"))
		return;
	cxl_proto_err_kfifo.work = work;
	cxl_proto_err_kfifo.flush = flush;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_register_proto_err_work, "cxl_core");

static struct work_struct *cancel_cxl_proto_err(void)
{
	struct work_struct *work;
	struct cxl_proto_err_work_data wd;

	guard(rwsem_write)(&cxl_proto_err_kfifo.rwsem);
	work = cxl_proto_err_kfifo.work;
	cxl_proto_err_kfifo.work = NULL;
	cxl_proto_err_kfifo.flush = NULL;

	/* rwsem_write excludes all producers; fifo_lock not needed */
	while (kfifo_get(&cxl_proto_err_kfifo.fifo, &wd)) {
		dev_err_ratelimited(&wd.pdev->dev,
				    "AER-CXL error report canceled\n");
		pci_dev_put(wd.pdev);
	}
	return work;
}

void cxl_unregister_proto_err_work(void)
{
	struct work_struct *work;

	lockdep_assert_not_held(&cxl_proto_err_kfifo.rwsem);

	work = cancel_cxl_proto_err();

	/* Wait for any in-flight cxl_proto_err_flush() calls to complete */
	wait_var_event(&cxl_proto_err_kfifo.flush_inflight,
		       atomic_read(&cxl_proto_err_kfifo.flush_inflight) == 0);

	if (work)
		cancel_work_sync(work);
}
EXPORT_SYMBOL_FOR_MODULES(cxl_unregister_proto_err_work, "cxl_core");

/**
 * for_each_cxl_proto_err - Call a function for each kfifo work item
 *
 * Single-consumer invariant: this function is only called from
 * cxl_proto_err_work_fn() via a single DECLARE_WORK.
 *
 * Holds rwsem_read internally; fn() must not call cxl_register_proto_err_work()
 * or cxl_unregister_proto_err_work().
 */
void for_each_cxl_proto_err(struct cxl_proto_err_work_data *wd,
			    cxl_proto_err_fn_t fn)
{
	guard(rwsem_read)(&cxl_proto_err_kfifo.rwsem);
	while (kfifo_get(&cxl_proto_err_kfifo.fifo, wd)) {
		fn(wd);
		pci_dev_put(wd->pdev);
	}
}
EXPORT_SYMBOL_FOR_MODULES(for_each_cxl_proto_err, "cxl_core");

/**
 * cxl_proto_err_flush - drain pending AER-CXL kfifo work synchronously
 *
 * Wait for the consumer worker to finish processing all entries
 * currently in the kfifo. Used by handle_error_source() for UCE so
 * the CXL plane can read CXL RAS, apply panic policy, and clear CXL
 * state before pci_aer_handle_error() drives PCIe recovery.
 *
 * Snapshots the flush callback under rwsem_read and releases the rwsem
 * before calling it.  This avoids holding rwsem_read across flush_work(),
 * which would deadlock via the rwsem HANDOFF mechanism when a concurrent
 * rwsem_write waiter (cxl_unregister_proto_err_work) blocks new readers
 * including the worker's for_each_cxl_proto_err() rwsem_read acquisition.
 *
 * The flush_inflight counter prevents cxl_core module unload while a
 * flush is in progress outside the rwsem. The counter is incremented
 * under rwsem_read (mutually exclusive with the rwsem_write in
 * cancel_cxl_proto_err() that NULLs the flush pointer) and decremented
 * after the flush completes. cxl_unregister_proto_err_work() waits for
 * the counter to reach zero before proceeding with cancel_work_sync().
 *
 * For correctable events the consumer can run asynchronously; AER
 * does not need to call this helper for AER_CORRECTABLE.
 */
void cxl_proto_err_flush(void)
{
	void (*flush)(void);

	scoped_guard(rwsem_read, &cxl_proto_err_kfifo.rwsem) {
		flush = cxl_proto_err_kfifo.flush;
		if (flush)
			atomic_inc(&cxl_proto_err_kfifo.flush_inflight);
	}

	if (flush) {
		flush();
		if (atomic_dec_and_test(&cxl_proto_err_kfifo.flush_inflight))
			wake_up_var(&cxl_proto_err_kfifo.flush_inflight);
	}
}
