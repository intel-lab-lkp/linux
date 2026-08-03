// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 AMD Corporation. All rights reserved. */

#include <linux/aer.h>
#include <linux/atomic.h>
#include <linux/cleanup.h>
#include <linux/init.h>
#include <linux/kfifo.h>
#include <linux/lockdep.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>
#include "../pci.h"
#include "portdrv.h"

#define CXL_ERROR_SOURCES_MAX          128

struct cxl_proto_err_kfifo {
	struct work_struct *work;
	void (*flush)(void);
	struct rw_semaphore rwsem;
	spinlock_t fifo_lock;           /* Serializes kfifo writers */
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

	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ENDPOINT)
		return false;

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
 * Return: true if the consumer workqueue was scheduled and the caller may
 * need to drain the kfifo before AER recovery; false if no CXL error
 * handling was initiated due to an early return on error (e.g. no kfifo
 * consumer registered).  Note that on a full kfifo a correctable error is
 * dropped but true is still returned; this is harmless because the caller
 * only drains the kfifo for non-correctable events.
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
	 * Reference discipline: the AER caller (handle_error_source()) holds
	 * a ref on @pdev for the duration of this call and releases it on
	 * return. Take a fresh ref here so the pdev stays live while queued
	 * in the kfifo; the corresponding consumer is for_each_cxl_proto_err()
	 * and will drop that ref after handling. On enqueue failure below,
	 * drop the ref we just took to avoid a leak.
	 */
	pci_dev_get(pdev);

	/* Serialize concurrent kfifo writers: multiple AER threaded IRQs */
	if (!kfifo_in_spinlocked(&cxl_proto_err_kfifo.fifo, &wd, 1,
				 &cxl_proto_err_kfifo.fifo_lock)) {

		if (info->severity != AER_CORRECTABLE) {
			/*
			 * Unlike PCIe AER, a dropped CXL.mem uncorrectable
			 * error cannot be treated as device-local: it may
			 * signal lost cache coherency over HDM memory in
			 * active use. The error can no longer be confirmed
			 * via CXL RAS, so collapse the unknown state to the
			 * same conservative outcome as a confirmed UCE.
			 */
			panic("CXL: dropped uncorrectable protocol error\n");
		}

		dev_err_ratelimited(&pdev->dev, "AER-CXL kfifo add failed\n");
		pci_dev_put(pdev);
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

	/* Wait for any in-flight cxl_proto_err_wait_for_empty() calls to complete */
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

		/* Corresponding ref incr taken in cxl_forward_error() */
		pci_dev_put(wd->pdev);
	}
}
EXPORT_SYMBOL_FOR_MODULES(for_each_cxl_proto_err, "cxl_core");

/**
 * cxl_proto_err_wait_for_empty - drain pending AER-CXL kfifo work synchronously
 *
 * Ensures CXL RAS handling and panic policy complete before AER
 * recovery proceeds. Only needed for UCE; CE runs asynchronously.
 *
 * Snapshots the flush callback under rwsem_read, then releases the
 * rwsem before calling it to avoid deadlock with a concurrent
 * rwsem_write from cxl_unregister_proto_err_work().
 *
 * The flush_inflight counter (typically 0 or 1) prevents module
 * unload while a flush is in progress outside the rwsem.
 */
void cxl_proto_err_wait_for_empty(void)
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
