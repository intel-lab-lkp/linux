// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights rsvd. */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <uapi/linux/idxd.h>
#include "idxd.h"
#include "registers.h"

static struct idxd_desc *__get_desc(struct idxd_wq *wq, int idx, int cpu)
{
	struct idxd_desc *desc;
	struct idxd_device *idxd = wq->idxd;

	desc = wq->descs[idx];
	memset(desc->hw, 0, sizeof(struct dsa_hw_desc));
	memset(desc->completion, 0, idxd->data->compl_size);
	desc->cpu = cpu;

	if (device_pasid_enabled(idxd))
		desc->hw->pasid = idxd->pasid;

	return desc;
}

struct idxd_desc *idxd_alloc_desc(struct idxd_wq *wq, enum idxd_op_type optype)
{
	int cpu, idx;
	struct idxd_device *idxd = wq->idxd;
	DEFINE_SBQ_WAIT(wait);
	struct sbq_wait_state *ws;
	struct sbitmap_queue *sbq;

	if (idxd->state != IDXD_DEV_ENABLED)
		return ERR_PTR(-EIO);
	/*
	 * A failed drain left descriptors the device may still complete.
	 * No slot is reused until a later drain completes or a halt kills
	 * them.
	 */
	if (READ_ONCE(wq->stuck))
		return ERR_PTR(-ENXIO);

	sbq = &wq->sbq;
	idx = sbitmap_queue_get(sbq, &cpu);
	if (idx < 0) {
		if (optype == IDXD_OP_NONBLOCK)
			return ERR_PTR(-EAGAIN);
	} else {
		return __get_desc(wq, idx, cpu);
	}

	/*
	 * A dying reference whose resurrect has completed belongs to a
	 * queue quiesced for good: no kill in progress will revive it, and
	 * every submission fails. Waiting for a slot there gains nothing
	 * and may never end. The teardown flush and the interrupt thread
	 * free the slot of each descriptor they complete only when its
	 * completion returns, and iaa_crypto's verify of a compression
	 * allocates from inside one. The two reads are not atomic, so an
	 * allocation that races a revive can fail like a request the kill
	 * caught.
	 */
	if (data_race(percpu_ref_is_dying(&wq->wq_active)) &&
	    completion_done(&wq->wq_resurrect))
		return ERR_PTR(-ENXIO);

	ws = &sbq->ws[0];
	for (;;) {
		sbitmap_prepare_to_wait(sbq, ws, &wait, TASK_INTERRUPTIBLE);
		if (signal_pending_state(TASK_INTERRUPTIBLE, current))
			break;
		idx = sbitmap_queue_get(sbq, &cpu);
		if (idx >= 0)
			break;
		schedule();
	}

	sbitmap_finish_wait(sbq, ws, &wait);
	if (idx < 0)
		return ERR_PTR(-EAGAIN);

	return __get_desc(wq, idx, cpu);
}
EXPORT_SYMBOL_NS_GPL(idxd_alloc_desc, "IDXD");

void idxd_free_desc(struct idxd_wq *wq, struct idxd_desc *desc)
{
	int cpu = desc->cpu;

	desc->cpu = -1;
	sbitmap_queue_clear(&wq->sbq, desc->id, cpu);
}
EXPORT_SYMBOL_NS_GPL(idxd_free_desc, "IDXD");

static struct idxd_desc *list_abort_desc(struct idxd_wq *wq, struct idxd_irq_entry *ie,
					 struct idxd_desc *desc)
{
	struct idxd_desc *d, *n;

	lockdep_assert_held(&ie->list_lock);
	list_for_each_entry_safe(d, n, &ie->work_list, list) {
		if (d == desc) {
			list_del(&d->list);
			return d;
		}
	}

	return NULL;
}

static void llist_abort_desc(struct idxd_wq *wq, struct idxd_irq_entry *ie,
			     struct idxd_desc *desc)
{
	struct idxd_desc *d, *t, *found = NULL;
	struct llist_node *head;
	bool wake = false;

	/*
	 * Grab the list lock so it will block the irq thread handler. This allows the
	 * abort code to locate the descriptor need to be aborted.
	 */
	spin_lock(&ie->list_lock);
	head = llist_del_all(&ie->pending_llist);
	if (head) {
		llist_for_each_entry_safe(d, t, head, llnode) {
			if (d == desc) {
				found = desc;
				continue;
			}

			/*
			 * A finished descriptor goes on the work list for
			 * the interrupt thread, as one still in flight does,
			 * rather than being completed in the submitter's
			 * context.
			 */
			if (d->completion->status)
				wake = true;
			list_add_tail(&d->list, &ie->work_list);
		}
	}

	if (!found)
		found = list_abort_desc(wq, ie, desc);
	spin_unlock(&ie->list_lock);

	/*
	 * Their interrupts may have run the thread before they reached the
	 * work list, so it is woken again.
	 */
	if (wake)
		irq_wake_thread(ie->vector, ie);

	/*
	 * The device never took the descriptor, so its record is still
	 * clear. The interrupt thread and another submitter's abort sort
	 * the lists under the list lock and leave one with a clear record
	 * on a list, so the search finds the descriptor, and once off the
	 * lists it is the submitter's alone. Marking it aborted before
	 * that would let another context that saw the mark on a list
	 * complete and free it, and the caller frees it as well.
	 */
	WARN_ON_ONCE(!found);
	/*
	 * Another driver's submitter cleans up all of its own: it learns
	 * of the failure from the return value and its descriptor carries
	 * its request where a dmaengine descriptor has its txd.
	 */
	if (found && idxd_wq_has_txd(wq))
		idxd_dma_complete_txd(desc, IDXD_COMPLETE_ABORT, false,
				      NULL, NULL);
}

/*
 * ENQCMDS typically fail when the WQ is inactive or busy. On host submission, the driver
 * has better control of number of descriptors being submitted to a shared wq by limiting
 * the number of driver allocated descriptors to the wq size. However, when the swq is
 * exported to a guest kernel, it may be shared with multiple guest kernels. This means
 * the likelihood of getting busy returned on the swq when submitting goes significantly up.
 * Having a tunable retry mechanism allows the driver to keep trying for a bit before giving
 * up. The sysfs knob can be tuned by the system administrator.
 */
int idxd_enqcmds(struct idxd_wq *wq, void __iomem *portal, const void *desc)
{
	unsigned int retries = wq->enqcmds_retries;
	int rc;

	do {
		rc = enqcmds(portal, desc);
		if (rc == 0)
			break;
		cpu_relax();
	} while (retries--);

	return rc;
}

int idxd_submit_desc(struct idxd_wq *wq, struct idxd_desc *desc)
{
	struct idxd_device *idxd = wq->idxd;
	struct idxd_irq_entry *ie = NULL;
	u32 desc_flags = desc->hw->flags;
	void __iomem *portal;
	int rc;

	if (idxd->state != IDXD_DEV_ENABLED)
		return -EIO;

	if (!percpu_ref_tryget_live(&wq->wq_active)) {
		wait_for_completion(&wq->wq_resurrect);
		if (!percpu_ref_tryget_live(&wq->wq_active))
			return -ENXIO;
	}

	portal = idxd_wq_portal_addr(wq);

	/*
	 * Pending the descriptor to the lockless list for the irq_entry
	 * that we designated the descriptor to.
	 */
	if (desc_flags & IDXD_OP_FLAG_RCI) {
		ie = &wq->ie;
		desc->hw->int_handle = ie->int_handle;
		llist_add(&desc->llnode, &ie->pending_llist);
	}

	/*
	 * The wmb() flushes writes to coherent DMA data before
	 * possibly triggering a DMA read. The wmb() is necessary
	 * even on UP because the recipient is a device.
	 */
	wmb();

	if (wq_dedicated(wq)) {
		iosubmit_cmds512(portal, desc->hw, 1);
	} else {
		rc = idxd_enqcmds(wq, portal, desc->hw);
		if (rc < 0) {
			percpu_ref_put(&wq->wq_active);
			/* abort operation frees the descriptor */
			if (ie)
				llist_abort_desc(wq, ie, desc);
			return rc;
		}
	}

	percpu_ref_put(&wq->wq_active);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(idxd_submit_desc, "IDXD");
