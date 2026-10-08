// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/pon.h>

#include "pon.h"

/**
 * DOC: The lent context
 *
 * A PON MAC runs the ITU-T activation state machine itself, but it must not
 * run it in hard interrupt context and it must not run it against a half
 * finished netlink transaction. The core therefore lends the driver its own
 * serialized context: one ordered workqueue per instance and one lock that
 * the netlink handlers take in their pre_doit.
 *
 * A driver queues a pon_work from any context, hard interrupt included. The
 * handler runs with pdev->lock held, so it may sleep.
 *
 * One worker drains the list one item at a time and re-queues itself while
 * the list is not empty, so the ordering a driver sees is the order it
 * queued in.
 */

void pon_work_worker(struct work_struct *work)
{
	struct pon_dev *pdev = container_of(work, struct pon_dev, work);
	struct pon_work *item;

	mutex_lock(&pdev->lock);

	if (pdev->going_away) {
		mutex_unlock(&pdev->lock);
		return;
	}

	spin_lock_irq(&pdev->work_lock);
	item = list_first_entry_or_null(&pdev->work_list, struct pon_work,
					entry);
	if (item) {
		list_del_init(&item->entry);
		if (!list_empty(&pdev->work_list))
			queue_work(pdev->wq, work);
	}
	spin_unlock_irq(&pdev->work_lock);

	if (item)
		item->func(pdev, item);

	mutex_unlock(&pdev->lock);
}

/**
 * pon_work_queue() - run a work item in the instance's context
 * @pdev: PON device structure
 * @work: the item, initialized with pon_work_init()
 *
 * Safe from any context, hard interrupt included. Queueing an item that is
 * already queued and has not yet run does nothing, so a burst of reports
 * costs one run.
 */
void pon_work_queue(struct pon_dev *pdev, struct pon_work *work)
{
	unsigned long flags;

	/*
	 * A driver that reports after pon_dev_unregister() canceled the work
	 * would queue onto a workqueue that is being drained, which warns. The
	 * instance is going: drop the item instead.
	 */
	if (READ_ONCE(pdev->going_away))
		return;

	spin_lock_irqsave(&pdev->work_lock, flags);
	if (list_empty(&work->entry))
		list_add_tail(&work->entry, &pdev->work_list);
	spin_unlock_irqrestore(&pdev->work_lock, flags);

	queue_work(pdev->wq, &pdev->work);
}
EXPORT_SYMBOL_GPL(pon_work_queue);

/**
 * pon_work_drain() - drop every queued item
 * @pdev: PON device structure
 *
 * For the unregister path, after @going_away is set. Items still on the list
 * are dropped rather than run, because the driver that owns them is leaving.
 */
void pon_work_drain(struct pon_dev *pdev)
{
	unsigned long flags;

	spin_lock_irqsave(&pdev->work_lock, flags);
	while (!list_empty(&pdev->work_list)) {
		struct pon_work *item;

		item = list_first_entry(&pdev->work_list, struct pon_work,
					entry);
		list_del_init(&item->entry);
	}
	spin_unlock_irqrestore(&pdev->work_lock, flags);
}
