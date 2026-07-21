// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#include "sw_fib.h"

#if IS_ENABLED(CONFIG_OCTEONTX_SWITCH)

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/switchdev.h>
#include <net/netevent.h>
#include <net/arp.h>
#include <net/route.h>

#include "../otx2_reg.h"
#include "../otx2_common.h"
#include "../otx2_struct.h"
#include "../cn10k.h"
#include "sw_nb.h"

#define SW_FIB_BATCH_MAX 16
#define SW_FIB_LIST_MAX 4096

/*
 * One switch PF registers notifiers via sw_nb_register(); a second call
 * returns -EBUSY. A single sw_fib_wq therefore serves the one switchdev
 * instance on octeontx2, matching the FDB offload path.
 */
static DEFINE_SPINLOCK(sw_fib_llock);
static LIST_HEAD(sw_fib_lh);
static atomic_t sw_fib_list_cnt = ATOMIC_INIT(0);

static struct workqueue_struct *sw_fib_wq;
static void sw_fib_work_handler(struct work_struct *work);
static DECLARE_DELAYED_WORK(sw_fib_work, sw_fib_work_handler);

struct sw_fib_list_entry {
	struct list_head lh;
	struct otx2_nic *pf;
	netdevice_tracker dev_tracker;
	int cnt;
	struct fib_entry *entry;
};

static void sw_fib_list_cnt_warn(struct net_device *netdev)
{
	int n = atomic_read(&sw_fib_list_cnt);

	if (n < 0)
		netdev_warn(netdev, "FIB list count underflow: %d\n", n);
	else if (n > SW_FIB_LIST_MAX)
		netdev_warn(netdev, "FIB list count overflow: %d (max %d)\n",
			    n, SW_FIB_LIST_MAX);
}

static int sw_fib_list_count(void)
{
	return atomic_read(&sw_fib_list_cnt);
}

static void sw_fib_list_cnt_inc(struct net_device *netdev)
{
	atomic_inc(&sw_fib_list_cnt);
	sw_fib_list_cnt_warn(netdev);
}

static void sw_fib_list_cnt_dec(struct net_device *netdev)
{
	atomic_dec(&sw_fib_list_cnt);
	sw_fib_list_cnt_warn(netdev);
}

static int sw_fib_notify(struct otx2_nic *pf,
			 int cnt,
			 struct fib_entry *entry)
{
	struct fib_notify_req *req;
	int rc;

	if (cnt > SW_FIB_BATCH_MAX)
		return -EINVAL;

	mutex_lock(&pf->mbox.lock);
	req = otx2_mbox_alloc_msg_fib_notify(&pf->mbox);
	if (!req) {
		rc = -ENOMEM;
		goto out;
	}

	req->cnt = cnt;
	memcpy(req->entry, entry, sizeof(*entry) * cnt);

	rc = otx2_sync_mbox_msg(&pf->mbox);
out:
	mutex_unlock(&pf->mbox.lock);
	return rc;
}

static void sw_fib_work_handler(struct work_struct *work)
{
	struct sw_fib_list_entry *lentry;
	LIST_HEAD(tlist);

	spin_lock_bh(&sw_fib_llock);
	list_splice_init(&sw_fib_lh, &tlist);
	spin_unlock_bh(&sw_fib_llock);

	while ((lentry =
		list_first_entry_or_null(&tlist,
					 struct sw_fib_list_entry, lh)) != NULL) {
		list_del_init(&lentry->lh);
		if (sw_fib_notify(lentry->pf, lentry->cnt, lentry->entry)) {
			netdev_err(lentry->pf->netdev,
				   "Failed to notify FIB update to AF, will retry\n");
			spin_lock_bh(&sw_fib_llock);
			if (sw_fib_wq) {
				list_add(&lentry->lh, &sw_fib_lh);
				queue_delayed_work(sw_fib_wq, &sw_fib_work,
						   msecs_to_jiffies(100));
				spin_unlock_bh(&sw_fib_llock);
				continue;
			}
			spin_unlock_bh(&sw_fib_llock);
			netdev_put(lentry->pf->netdev, &lentry->dev_tracker);
			sw_fib_list_cnt_dec(lentry->pf->netdev);
			kfree(lentry->entry);
			kfree(lentry);
			continue;
		}
		sw_fib_list_cnt_dec(lentry->pf->netdev);
		netdev_put(lentry->pf->netdev, &lentry->dev_tracker);
		kfree(lentry->entry);
		kfree(lentry);
	}

	spin_lock_bh(&sw_fib_llock);
	if (!list_empty(&sw_fib_lh) && sw_fib_wq)
		queue_delayed_work(sw_fib_wq, &sw_fib_work,
				   msecs_to_jiffies(10));
	spin_unlock_bh(&sw_fib_llock);
}

int sw_fib_add_to_list(struct net_device *dev,
		       struct fib_entry *entry, int cnt)
{
	struct otx2_nic *pf = netdev_priv(dev);
	struct sw_fib_list_entry *lentry;
	struct workqueue_struct *wq;

	if (cnt <= 0 || cnt > SW_FIB_BATCH_MAX) {
		kfree(entry);
		return -EINVAL;
	}

	spin_lock_bh(&sw_fib_llock);
	if (!sw_fib_wq) {
		spin_unlock_bh(&sw_fib_llock);
		kfree(entry);
		return -EINVAL;
	}
	spin_unlock_bh(&sw_fib_llock);

	if (sw_fib_list_count() >= SW_FIB_LIST_MAX) {
		kfree(entry);
		return -ENOMEM;
	}

	lentry = kcalloc(1, sizeof(*lentry), GFP_ATOMIC);
	if (!lentry) {
		kfree(entry);
		return -ENOMEM;
	}

	lentry->pf = pf;
	lentry->cnt = cnt;
	lentry->entry = entry;
	INIT_LIST_HEAD(&lentry->lh);
	netdev_hold(dev, &lentry->dev_tracker, GFP_ATOMIC);

	spin_lock_bh(&sw_fib_llock);
	wq = sw_fib_wq;
	if (wq) {
		list_add_tail(&lentry->lh, &sw_fib_lh);
		sw_fib_list_cnt_inc(dev);
		queue_delayed_work(wq, &sw_fib_work,
				   msecs_to_jiffies(10));
	}
	spin_unlock_bh(&sw_fib_llock);

	if (!wq) {
		netdev_put(dev, &lentry->dev_tracker);
		kfree(lentry);
		kfree(entry);
		return -EINVAL;
	}

	return 0;
}

int sw_fib_init(void)
{
	sw_fib_wq = alloc_workqueue("sw_pf_fib_wq", 0, 0);
	if (!sw_fib_wq)
		return -ENOMEM;

	return 0;
}

void sw_fib_deinit(void)
{
	struct sw_fib_list_entry *lentry;
	struct workqueue_struct *wq;
	LIST_HEAD(tlist);

	spin_lock_bh(&sw_fib_llock);
	wq = sw_fib_wq;
	sw_fib_wq = NULL;
	spin_unlock_bh(&sw_fib_llock);

	if (!wq)
		return;

	cancel_delayed_work_sync(&sw_fib_work);
	destroy_workqueue(wq);

	spin_lock_bh(&sw_fib_llock);
	list_splice_init(&sw_fib_lh, &tlist);
	spin_unlock_bh(&sw_fib_llock);

	while ((lentry =
		list_first_entry_or_null(&tlist,
					 struct sw_fib_list_entry, lh)) != NULL) {
		list_del_init(&lentry->lh);
		sw_fib_list_cnt_dec(lentry->pf->netdev);
		netdev_put(lentry->pf->netdev, &lentry->dev_tracker);
		kfree(lentry->entry);
		kfree(lentry);
	}
}

#endif
