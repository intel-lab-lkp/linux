// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU Admin Function driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */

#include <linux/bitfield.h>
#include "rvu.h"
#include "rvu_sw.h"
#include "rvu_sw_l2.h"

#define M(_name, _id, _fn_name, _req_type, _rsp_type)			\
static struct _req_type __maybe_unused					\
*otx2_mbox_alloc_msg_ ## _fn_name(struct rvu *rvu, int devid)		\
{									\
	struct _req_type *req;						\
									\
	req = (struct _req_type *)otx2_mbox_alloc_msg_rsp(		\
		&rvu->afpf_wq_info.mbox_up, devid, sizeof(struct _req_type), \
		sizeof(struct _rsp_type));				\
	if (!req)							\
		return NULL;						\
	req->hdr.sig = OTX2_MBOX_REQ_SIG;				\
	req->hdr.id = _id;						\
	return req;							\
}
MBOX_UP_AF2SWDEV_MESSAGES
MBOX_UP_AF2PF_FDB_REFRESH_MESSAGES
#undef M

#define RVU_SW_L2_LIST_MAX 4096

struct l2_entry {
	struct list_head list;
	u64 flags;
	u32 port_id;
	u8  mac[ETH_ALEN];
};

static DEFINE_MUTEX(l2_offl_list_lock);
static LIST_HEAD(l2_offl_lh);
static atomic_t l2_offl_list_cnt = ATOMIC_INIT(0);

static DEFINE_MUTEX(fdb_refresh_list_lock);
static LIST_HEAD(fdb_refresh_lh);
static atomic_t fdb_refresh_list_cnt = ATOMIC_INIT(0);

struct rvu_sw_l2_work {
	struct rvu *rvu;
	struct work_struct work;
};

/* Work queue for switchdev message handling. There is only
 * one switch HW per SoC, so one instance of each type of
 * workqueue is enough.
 */
static struct rvu_sw_l2_work l2_offl_work;
static struct workqueue_struct *rvu_sw_l2_offl_wq;

static struct rvu_sw_l2_work fdb_refresh_work;
static struct workqueue_struct *fdb_refresh_wq;

static bool fw_is_up;
static DEFINE_SPINLOCK(rvu_sw_l2_state_lock);

static void rvu_sw_l2_list_cnt_warn(struct device *dev, atomic_t *cnt,
				    const char *name)
{
	int n = atomic_read(cnt);

	if (n < 0)
		dev_warn(dev, "L2 %s list count underflow: %d\n", name, n);
	else if (n > RVU_SW_L2_LIST_MAX)
		dev_warn(dev, "L2 %s list count overflow: %d (max %d)\n",
			 name, n, RVU_SW_L2_LIST_MAX);
}

static void rvu_sw_l2_list_cnt_inc(struct device *dev, atomic_t *cnt,
				   const char *name)
{
	atomic_inc(cnt);
	rvu_sw_l2_list_cnt_warn(dev, cnt, name);
}

static void rvu_sw_l2_list_cnt_dec(struct device *dev, atomic_t *cnt,
				   const char *name)
{
	atomic_dec(cnt);
	rvu_sw_l2_list_cnt_warn(dev, cnt, name);
}

static void rvu_sw_l2_destroy_wqs(struct rvu *rvu)
{
	struct workqueue_struct *offl_wq, *refresh_wq;
	struct l2_entry *entry;

	spin_lock_bh(&rvu_sw_l2_state_lock);
	rvu->rswitch.flags &= ~RVU_SWITCH_FLAG_FW_READY;
	rvu->rswitch.pcifunc = 0;
	fw_is_up = false;
	offl_wq = rvu_sw_l2_offl_wq;
	refresh_wq = fdb_refresh_wq;
	rvu_sw_l2_offl_wq = NULL;
	fdb_refresh_wq = NULL;
	spin_unlock_bh(&rvu_sw_l2_state_lock);

	if (refresh_wq) {
		cancel_work_sync(&fdb_refresh_work.work);
		destroy_workqueue(refresh_wq);

		mutex_lock(&fdb_refresh_list_lock);
		rvu_sw_l2_list_cnt_warn(rvu->dev, &fdb_refresh_list_cnt,
					"fdb refresh");
		while (1) {
			entry = list_first_entry_or_null(&fdb_refresh_lh,
							 struct l2_entry, list);
			if (!entry)
				break;

			list_del_init(&entry->list);
			kfree(entry);
		}
		atomic_set(&fdb_refresh_list_cnt, 0);
		mutex_unlock(&fdb_refresh_list_lock);
	}

	if (offl_wq) {
		cancel_work_sync(&l2_offl_work.work);
		destroy_workqueue(offl_wq);

		mutex_lock(&l2_offl_list_lock);
		rvu_sw_l2_list_cnt_warn(rvu->dev, &l2_offl_list_cnt, "offload");
		while (1) {
			entry = list_first_entry_or_null(&l2_offl_lh,
							 struct l2_entry, list);
			if (!entry)
				break;

			list_del_init(&entry->list);
			kfree(entry);
		}
		atomic_set(&l2_offl_list_cnt, 0);
		mutex_unlock(&l2_offl_list_lock);
	}
}

/* High-frequency link state transitions or aggressive FDB
 * aging intervals can induce rapid fdb churn. To prevent
 * thrashing, inhibit hardware offloading of these transient
 * forwarding states to the switching ASIC.  When processing an ADD,
 * drop a queued DELETE for the same MAC that has not yet been sent to
 * hardware; the ADD reflects the desired final state and supersedes it.
 */
static void rvu_sw_l2_offl_drop_pending_del(u8 *mac)
{
	struct l2_entry *entry, *tmp;

	mutex_lock(&l2_offl_list_lock);
	list_for_each_entry_safe(entry, tmp, &l2_offl_lh, list) {
		if (!ether_addr_equal(mac, entry->mac))
			continue;

		if (!(entry->flags & FDB_DEL))
			continue;

		list_del_init(&entry->list);
		rvu_sw_l2_list_cnt_dec(l2_offl_work.rvu->dev, &l2_offl_list_cnt,
				       "offload");
		kfree(entry);
		break;
	}
	mutex_unlock(&l2_offl_list_lock);
}

static int rvu_sw_l2_offl_rule_push(struct rvu *rvu, struct l2_entry *l2_entry)
{
	struct af2swdev_notify_req *req;
	int swdev_pf;

	swdev_pf = rvu_get_pf(rvu->pdev, rvu->rswitch.pcifunc);

	mutex_lock(&rvu->mbox_lock);
	req = otx2_mbox_alloc_msg_af2swdev_notify(rvu, swdev_pf);
	if (!req) {
		mutex_unlock(&rvu->mbox_lock);
		return -ENOMEM;
	}

	ether_addr_copy(req->mac, l2_entry->mac);
	req->flags = l2_entry->flags;
	req->port_id = l2_entry->port_id;

	otx2_mbox_wait_for_zero(&rvu->afpf_wq_info.mbox_up, swdev_pf);
	otx2_mbox_msg_send_up(&rvu->afpf_wq_info.mbox_up, swdev_pf);

	mutex_unlock(&rvu->mbox_lock);
	return 0;
}

static int rvu_sw_l2_fdb_refresh_send(struct rvu *rvu, u16 pcifunc, u8 *mac)
{
	struct af2pf_fdb_refresh_req *req;
	int pf, vidx;

	if (!is_pf_func_valid(rvu, pcifunc))
		return -EINVAL;

	pf = rvu_get_pf(rvu->pdev, pcifunc);

	mutex_lock(&rvu->mbox_lock);

	if (pf) {
		if (pf >= rvu->afpf_wq_info.mbox_up.ndevs) {
			mutex_unlock(&rvu->mbox_lock);
			return -EINVAL;
		}

		req = otx2_mbox_alloc_msg_af2pf_fdb_refresh(rvu, pf);
		if (!req) {
			mutex_unlock(&rvu->mbox_lock);
			return -ENOMEM;
		}

		req->hdr.pcifunc = pcifunc;
		ether_addr_copy(req->mac, mac);
		req->pcifunc = pcifunc;
		req->flags = FDB_ADD;

		otx2_mbox_wait_for_zero(&rvu->afpf_wq_info.mbox_up, pf);
		otx2_mbox_msg_send_up(&rvu->afpf_wq_info.mbox_up, pf);
	} else {
		vidx = pcifunc - 1;

		if (vidx < 0 || vidx >= rvu->afvf_wq_info.mbox_up.ndevs) {
			mutex_unlock(&rvu->mbox_lock);
			return -EINVAL;
		}

		req = (struct af2pf_fdb_refresh_req *)
			otx2_mbox_alloc_msg_rsp(&rvu->afvf_wq_info.mbox_up, vidx,
						sizeof(*req), sizeof(struct msg_rsp));
		if (!req) {
			mutex_unlock(&rvu->mbox_lock);
			return -ENOMEM;
		}
		req->hdr.sig = OTX2_MBOX_REQ_SIG;
		req->hdr.id = MBOX_MSG_AF2PF_FDB_REFRESH;

		req->hdr.pcifunc = pcifunc;
		ether_addr_copy(req->mac, mac);
		req->pcifunc = pcifunc;
		req->flags = FDB_ADD;

		otx2_mbox_wait_for_zero(&rvu->afvf_wq_info.mbox_up, vidx);
		otx2_mbox_msg_send_up(&rvu->afvf_wq_info.mbox_up, vidx);
	}

	mutex_unlock(&rvu->mbox_lock);

	return 0;
}

static void rvu_sw_l2_fdb_refresh_wq_handler(struct work_struct *work)
{
	struct rvu_sw_l2_work *fdb_work;
	struct l2_entry *l2_entry;

	fdb_work = container_of(work, struct rvu_sw_l2_work, work);

	while (1) {
		mutex_lock(&fdb_refresh_list_lock);
		l2_entry = list_first_entry_or_null(&fdb_refresh_lh,
						    struct l2_entry, list);
		if (!l2_entry) {
			mutex_unlock(&fdb_refresh_list_lock);
			return;
		}

		list_del_init(&l2_entry->list);
		rvu_sw_l2_list_cnt_dec(fdb_work->rvu->dev, &fdb_refresh_list_cnt,
				       "fdb refresh");
		mutex_unlock(&fdb_refresh_list_lock);

		rvu_sw_l2_fdb_refresh_send(fdb_work->rvu, l2_entry->port_id,
					   l2_entry->mac);
		kfree(l2_entry);
	}
}

static void rvu_sw_l2_offl_rule_wq_handler(struct work_struct *work)
{
	struct rvu_sw_l2_work *offl_work;
	struct l2_entry *l2_entry;
	int budget = 16;
	bool add_fdb;

	offl_work = container_of(work, struct rvu_sw_l2_work, work);

	while (budget--) {
		mutex_lock(&l2_offl_list_lock);
		l2_entry = list_first_entry_or_null(&l2_offl_lh, struct l2_entry, list);
		if (!l2_entry) {
			mutex_unlock(&l2_offl_list_lock);
			return;
		}

		list_del_init(&l2_entry->list);
		rvu_sw_l2_list_cnt_dec(offl_work->rvu->dev, &l2_offl_list_cnt,
				       "offload");
		mutex_unlock(&l2_offl_list_lock);

		add_fdb = !!(l2_entry->flags & FDB_ADD);

		if (add_fdb)
			rvu_sw_l2_offl_drop_pending_del(l2_entry->mac);

		if (rvu_sw_l2_offl_rule_push(offl_work->rvu, l2_entry))
			dev_err(offl_work->rvu->dev,
				"%s: Error to push l2 rule\n",
				__func__);
		kfree(l2_entry);
	}

	spin_lock_bh(&rvu_sw_l2_state_lock);
	if (rvu_sw_l2_offl_wq && atomic_read(&l2_offl_list_cnt))
		queue_work(rvu_sw_l2_offl_wq, &l2_offl_work.work);
	spin_unlock_bh(&rvu_sw_l2_state_lock);
}

int rvu_sw_l2_init_offl_wq(struct rvu *rvu, u16 pcifunc, bool fw_up)
{
	struct rvu_switch *rswitch = &rvu->rswitch;

	if (!fw_up) {
		rvu_sw_l2_destroy_wqs(rvu);
		return 0;
	}

	spin_lock_bh(&rvu_sw_l2_state_lock);
	if (fw_is_up && rvu_sw_l2_offl_wq && fdb_refresh_wq) {
		rswitch->pcifunc = pcifunc;
		rswitch->flags |= RVU_SWITCH_FLAG_FW_READY;
		spin_unlock_bh(&rvu_sw_l2_state_lock);
		return 0;
	}
	spin_unlock_bh(&rvu_sw_l2_state_lock);

	if (rvu_sw_l2_offl_wq || fdb_refresh_wq)
		rvu_sw_l2_destroy_wqs(rvu);

	l2_offl_work.rvu = rvu;
	INIT_WORK(&l2_offl_work.work, rvu_sw_l2_offl_rule_wq_handler);
	rvu_sw_l2_offl_wq = alloc_workqueue("swdev_rvu_sw_l2_offl_wq", 0, 0);
	if (!rvu_sw_l2_offl_wq) {
		dev_err(rvu->dev, "L2 offl workqueue allocation failed\n");
		return -ENOMEM;
	}

	fdb_refresh_work.rvu = rvu;
	INIT_WORK(&fdb_refresh_work.work, rvu_sw_l2_fdb_refresh_wq_handler);
	fdb_refresh_wq = alloc_workqueue("swdev_fdb_refresh_wq", 0, 0);
	if (!fdb_refresh_wq) {
		dev_err(rvu->dev, "fdb refresh workqueue allocation failed\n");
		destroy_workqueue(rvu_sw_l2_offl_wq);
		rvu_sw_l2_offl_wq = NULL;
		return -ENOMEM;
	}

	spin_lock_bh(&rvu_sw_l2_state_lock);
	fw_is_up = true;
	rswitch->pcifunc = pcifunc;
	rswitch->flags |= RVU_SWITCH_FLAG_FW_READY;
	spin_unlock_bh(&rvu_sw_l2_state_lock);

	return 0;
}

int rvu_sw_l2_fdb_list_entry_add(struct rvu *rvu, u16 pcifunc, u8 *mac)
{
	struct workqueue_struct *wq;
	struct l2_entry *l2_entry;

	if (!is_pf_func_valid(rvu, pcifunc))
		return -EINVAL;

	spin_lock_bh(&rvu_sw_l2_state_lock);
	if (!fdb_refresh_wq) {
		spin_unlock_bh(&rvu_sw_l2_state_lock);
		return -EINVAL;
	}
	spin_unlock_bh(&rvu_sw_l2_state_lock);

	if (atomic_read(&fdb_refresh_list_cnt) >= RVU_SW_L2_LIST_MAX) {
		rvu_sw_l2_list_cnt_warn(rvu->dev, &fdb_refresh_list_cnt,
					"fdb refresh");
		return -ENOMEM;
	}

	l2_entry = kcalloc(1, sizeof(*l2_entry), GFP_KERNEL);
	if (!l2_entry)
		return -ENOMEM;

	l2_entry->port_id = pcifunc;
	ether_addr_copy(l2_entry->mac, mac);

	mutex_lock(&fdb_refresh_list_lock);
	if (atomic_read(&fdb_refresh_list_cnt) >= RVU_SW_L2_LIST_MAX) {
		rvu_sw_l2_list_cnt_warn(rvu->dev, &fdb_refresh_list_cnt,
					"fdb refresh");
		mutex_unlock(&fdb_refresh_list_lock);
		kfree(l2_entry);
		return -ENOMEM;
	}
	list_add_tail(&l2_entry->list, &fdb_refresh_lh);
	rvu_sw_l2_list_cnt_inc(rvu->dev, &fdb_refresh_list_cnt, "fdb refresh");
	mutex_unlock(&fdb_refresh_list_lock);

	spin_lock_bh(&rvu_sw_l2_state_lock);
	wq = fdb_refresh_wq;
	if (wq)
		queue_work(wq, &fdb_refresh_work.work);
	spin_unlock_bh(&rvu_sw_l2_state_lock);

	if (!wq) {
		mutex_lock(&fdb_refresh_list_lock);
		list_del_init(&l2_entry->list);
		rvu_sw_l2_list_cnt_dec(rvu->dev, &fdb_refresh_list_cnt,
				       "fdb refresh");
		mutex_unlock(&fdb_refresh_list_lock);
		kfree(l2_entry);
		return -EINVAL;
	}

	return 0;
}

int rvu_mbox_handler_fdb_notify(struct rvu *rvu,
				struct fdb_notify_req *req,
				struct msg_rsp *rsp)
{
	struct workqueue_struct *wq;
	struct l2_entry *l2_entry;

	spin_lock_bh(&rvu_sw_l2_state_lock);
	if (!(rvu->rswitch.flags & RVU_SWITCH_FLAG_FW_READY) ||
	    !rvu_sw_l2_offl_wq) {
		spin_unlock_bh(&rvu_sw_l2_state_lock);
		return 0;
	}
	spin_unlock_bh(&rvu_sw_l2_state_lock);

	if (atomic_read(&l2_offl_list_cnt) >= RVU_SW_L2_LIST_MAX) {
		rvu_sw_l2_list_cnt_warn(rvu->dev, &l2_offl_list_cnt, "offload");
		return -ENOMEM;
	}

	l2_entry = kcalloc(1, sizeof(*l2_entry), GFP_KERNEL);
	if (!l2_entry)
		return -ENOMEM;

	l2_entry->port_id = rvu_sw_port_id(rvu, req->hdr.pcifunc);
	ether_addr_copy(l2_entry->mac, req->mac);
	l2_entry->flags = req->flags;

	mutex_lock(&l2_offl_list_lock);
	if (atomic_read(&l2_offl_list_cnt) >= RVU_SW_L2_LIST_MAX) {
		rvu_sw_l2_list_cnt_warn(rvu->dev, &l2_offl_list_cnt, "offload");
		mutex_unlock(&l2_offl_list_lock);
		kfree(l2_entry);
		return -ENOMEM;
	}
	list_add_tail(&l2_entry->list, &l2_offl_lh);
	rvu_sw_l2_list_cnt_inc(rvu->dev, &l2_offl_list_cnt, "offload");
	mutex_unlock(&l2_offl_list_lock);

	spin_lock_bh(&rvu_sw_l2_state_lock);
	wq = rvu_sw_l2_offl_wq;
	if (wq)
		queue_work(wq, &l2_offl_work.work);
	spin_unlock_bh(&rvu_sw_l2_state_lock);

	if (!wq) {
		mutex_lock(&l2_offl_list_lock);
		list_del_init(&l2_entry->list);
		rvu_sw_l2_list_cnt_dec(rvu->dev, &l2_offl_list_cnt, "offload");
		mutex_unlock(&l2_offl_list_lock);
		kfree(l2_entry);
	}

	return 0;
}

void rvu_sw_l2_shutdown(void)
{
	if (!fdb_refresh_wq && !rvu_sw_l2_offl_wq)
		return;

	rvu_sw_l2_destroy_wqs(l2_offl_work.rvu);
}
