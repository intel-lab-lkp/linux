// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/switchdev.h>
#include <net/netevent.h>
#include <net/arp.h>

#include "../otx2_reg.h"
#include "../otx2_common.h"
#include "../otx2_struct.h"
#include "../cn10k.h"
#include "sw_nb.h"
#include "sw_fdb.h"

#if !IS_ENABLED(CONFIG_OCTEONTX_SWITCH)

int otx2_mbox_up_handler_af2pf_fdb_refresh(struct otx2_nic *pf,
					   struct af2pf_fdb_refresh_req *req,
					   struct msg_rsp *rsp)
{
	return 0;
}

#else

#define SW_FDB_LIST_MAX 4096

static DEFINE_SPINLOCK(sw_fdb_llock);
static LIST_HEAD(sw_fdb_lh);
static atomic_t sw_fdb_list_cnt = ATOMIC_INIT(0);

struct sw_fdb_list_entry {
	struct list_head list;
	u64 flags;
	struct otx2_nic *pf;
	netdevice_tracker dev_tracker;
	u8  mac[ETH_ALEN];
	bool add_fdb;
};

static struct workqueue_struct *sw_fdb_wq;
static struct work_struct sw_fdb_work;

static void sw_fdb_list_cnt_warn(struct net_device *netdev)
{
	int n = atomic_read(&sw_fdb_list_cnt);

	if (n < 0)
		netdev_warn(netdev, "FDB list count underflow: %d\n", n);
	else if (n > SW_FDB_LIST_MAX)
		netdev_warn(netdev, "FDB list count overflow: %d (max %d)\n",
			    n, SW_FDB_LIST_MAX);
}

static int sw_fdb_list_count(void)
{
	return atomic_read(&sw_fdb_list_cnt);
}

static void sw_fdb_list_cnt_inc(struct net_device *netdev)
{
	atomic_inc(&sw_fdb_list_cnt);
	sw_fdb_list_cnt_warn(netdev);
}

static void sw_fdb_list_cnt_dec(struct net_device *netdev)
{
	atomic_dec(&sw_fdb_list_cnt);
	sw_fdb_list_cnt_warn(netdev);
}

static int sw_fdb_add_or_del(struct otx2_nic *pf,
			     const unsigned char *addr,
			     bool add_fdb)
{
	struct fdb_notify_req *req;
	int rc;

	mutex_lock(&pf->mbox.lock);
	req = otx2_mbox_alloc_msg_fdb_notify(&pf->mbox);
	if (!req) {
		rc = -ENOMEM;
		goto out;
	}

	ether_addr_copy(req->mac, addr);
	req->flags = add_fdb ? FDB_ADD : FDB_DEL;

	rc = otx2_sync_mbox_msg(&pf->mbox);
out:
	mutex_unlock(&pf->mbox.lock);
	return rc;
}

static void sw_fdb_wq_handler(struct work_struct *work)
{
	struct sw_fdb_list_entry *entry;
	struct workqueue_struct *wq;
	LIST_HEAD(tlist);

	spin_lock_bh(&sw_fdb_llock);
	list_splice_init(&sw_fdb_lh, &tlist);
	spin_unlock_bh(&sw_fdb_llock);

	while ((entry =
		list_first_entry_or_null(&tlist,
					 struct sw_fdb_list_entry,
					 list)) != NULL) {
		list_del_init(&entry->list);
		sw_fdb_list_cnt_dec(entry->pf->netdev);
		if (sw_fdb_add_or_del(entry->pf, entry->mac, entry->add_fdb))
			netdev_err(entry->pf->netdev,
				   "Error to add/del fdb %pM entry\n",
				   entry->mac);
		netdev_put(entry->pf->netdev, &entry->dev_tracker);
		kfree(entry);
	}

	spin_lock_bh(&sw_fdb_llock);
	wq = sw_fdb_wq;
	if (wq && !list_empty(&sw_fdb_lh))
		queue_work(wq, &sw_fdb_work);
	spin_unlock_bh(&sw_fdb_llock);
}

int sw_fdb_add_to_list(struct net_device *dev, u8 *mac, bool add_fdb)
{
	struct otx2_nic *pf = netdev_priv(dev);
	struct sw_fdb_list_entry *entry;
	struct workqueue_struct *wq;

	spin_lock_bh(&sw_fdb_llock);
	if (!sw_fdb_wq) {
		spin_unlock_bh(&sw_fdb_llock);
		return -EINVAL;
	}
	spin_unlock_bh(&sw_fdb_llock);

	if (sw_fdb_list_count() >= SW_FDB_LIST_MAX)
		return -ENOMEM;

	entry = kcalloc(1, sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return -ENOMEM;

	ether_addr_copy(entry->mac, mac);
	entry->add_fdb = add_fdb;
	entry->pf = pf;
	netdev_hold(dev, &entry->dev_tracker, GFP_ATOMIC);

	spin_lock_bh(&sw_fdb_llock);
	wq = sw_fdb_wq;
	if (wq) {
		list_add_tail(&entry->list, &sw_fdb_lh);
		sw_fdb_list_cnt_inc(dev);
		queue_work(wq, &sw_fdb_work);
	}
	spin_unlock_bh(&sw_fdb_llock);

	if (!wq) {
		netdev_put(dev, &entry->dev_tracker);
		kfree(entry);
		return -EINVAL;
	}

	return 0;
}

int sw_fdb_init(void)
{
	INIT_WORK(&sw_fdb_work, sw_fdb_wq_handler);
	sw_fdb_wq = alloc_workqueue("sw_fdb_wq", 0, 0);
	if (!sw_fdb_wq)
		return -ENOMEM;

	return 0;
}

void sw_fdb_deinit(void)
{
	struct sw_fdb_list_entry *entry;
	struct workqueue_struct *wq;
	LIST_HEAD(tlist);

	spin_lock_bh(&sw_fdb_llock);
	wq = sw_fdb_wq;
	sw_fdb_wq = NULL;
	spin_unlock_bh(&sw_fdb_llock);

	if (!wq)
		return;

	cancel_work_sync(&sw_fdb_work);
	destroy_workqueue(wq);

	spin_lock_bh(&sw_fdb_llock);
	list_splice_init(&sw_fdb_lh, &tlist);
	spin_unlock_bh(&sw_fdb_llock);

	while ((entry =
		list_first_entry_or_null(&tlist,
					 struct sw_fdb_list_entry,
					 list)) != NULL) {
		list_del_init(&entry->list);
		sw_fdb_list_cnt_dec(entry->pf->netdev);
		netdev_put(entry->pf->netdev, &entry->dev_tracker);
		kfree(entry);
	}
}

int otx2_mbox_up_handler_af2pf_fdb_refresh(struct otx2_nic *pf,
					   struct af2pf_fdb_refresh_req *req,
					   struct msg_rsp *rsp)
{
	struct switchdev_notifier_fdb_info item = {0};

	/* FDB refresh is raised from the switch offload path (AF) after
	 * switchdev FDB updates and is delivered to the PF mailbox.
	 * Refreshes targeting the PF netdev are applied here on
	 * pf->netdev; VF-targeted refreshes are forwarded on the PF-VF
	 * mailbox and handled in otx2vf_mbox_af2pf_fdb_refresh() on
	 * vf->netdev (see rvu_sw_l2_fdb_refresh_send()).
	 */
	item.addr = req->mac;
	item.info.dev = pf->netdev;
	if (req->flags & FDB_DEL)
		call_switchdev_notifiers(SWITCHDEV_FDB_DEL_TO_BRIDGE,
					 item.info.dev, &item.info, NULL);
	else
		call_switchdev_notifiers(SWITCHDEV_FDB_ADD_TO_BRIDGE,
					 item.info.dev, &item.info, NULL);

	return 0;
}
#endif
