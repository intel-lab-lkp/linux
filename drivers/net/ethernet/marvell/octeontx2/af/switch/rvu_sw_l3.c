// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU Admin Function driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */

#include <linux/bitfield.h>
#include "rvu.h"
#include "rvu_sw.h"
#include "rvu_sw_l3.h"

static struct af2swdev_notify_req __maybe_unused
*otx2_mbox_alloc_msg_af2swdev_notify(struct rvu *rvu, int devid)
{
	struct af2swdev_notify_req *req;

	req = (struct af2swdev_notify_req *)
		otx2_mbox_alloc_msg_rsp(&rvu->afpf_wq_info.mbox_up, devid,
					sizeof(*req), sizeof(struct msg_rsp));
	if (!req)
		return NULL;
	req->hdr.sig = OTX2_MBOX_REQ_SIG;
	req->hdr.id = MBOX_MSG_AF2SWDEV;
	return req;
}

#define RVU_SW_L3_BATCH_MAX						\
	((int)(sizeof_field(struct af2swdev_notify_req, entry) /	\
	       sizeof(struct fib_entry)))

struct l3_entry {
	struct list_head list;
	/* Always this AF driver's rvu; stored for clarity only (single RVU). */
	struct rvu *rvu;
	u32 port_id;
	int cnt;
	struct fib_entry entry[];
};

static DEFINE_MUTEX(l3_offl_llock);
static LIST_HEAD(l3_offl_lh);

static struct workqueue_struct *sw_l3_offl_wq;
static void sw_l3_offl_work_handler(struct work_struct *work);
static DECLARE_DELAYED_WORK(l3_offl_work, sw_l3_offl_work_handler);

/*
 * FIB offload to the switch ASIC: one octeontx2 AF driver instance, one
 * switch PF (switchdev), and one sw_l3_offl_wq per SoC.
 */

static void rvu_sw_l3_drain_list(struct list_head *lh)
{
	struct l3_entry *entry;

	while ((entry = list_first_entry_or_null(lh, struct l3_entry, list))) {
		list_del(&entry->list);
		kfree(entry);
	}
}

static void rvu_sw_l3_queue_work(void)
{
	if (sw_l3_offl_wq)
		queue_delayed_work(sw_l3_offl_wq, &l3_offl_work,
				   msecs_to_jiffies(10));
}

static int rvu_sw_l3_ensure_wq(void)
{
	if (sw_l3_offl_wq)
		return 0;

	sw_l3_offl_wq = alloc_workqueue("sw_af_fib_wq", 0, 0);
	if (!sw_l3_offl_wq)
		return -ENOMEM;

	return 0;
}

static int rvu_sw_l3_offl_rule_push(struct list_head *lh)
{
	struct af2swdev_notify_req *req;
	struct fib_entry *entry, *dst;
	struct l3_entry *l3_entry;
	struct rvu *rvu;
	int tot_cnt = 0;
	int swdev_pf;
	int sz, cnt, i;
	bool rc;

	BUILD_BUG_ON(sizeof_field(struct af2swdev_notify_req, entry) !=
		     sizeof(struct fib_entry) * RVU_SW_L3_BATCH_MAX);

	l3_entry = list_first_entry_or_null(lh, struct l3_entry, list);
	if (!l3_entry)
		return 0;

	/*
	 * Octeontx2 has a single AF (one struct rvu) per RVU chip. All queued
	 * entries therefore share the same rvu and the same switch PF below.
	 * Host PF identity is carried per fib_entry (port_id), not by picking
	 * a different switch PF here.
	 */
	rvu = l3_entry->rvu;
	swdev_pf = rvu_get_pf(rvu->pdev, rvu->rswitch.pcifunc);

	mutex_lock(&rvu->mbox_lock);
	req = otx2_mbox_alloc_msg_af2swdev_notify(rvu, swdev_pf);
	if (!req) {
		mutex_unlock(&rvu->mbox_lock);
		return -ENOMEM;
	}

	dst = &req->entry[0];
	/*
	 * Batch fib_entry records from multiple host PF notifies into one
	 * af2swdev message. Safe on octeontx2: every l3_entry targets the
	 * same switch PF; egress port is encoded in each fib_entry.port_id.
	 *
	 * Entries are removed from lh and freed once copied into the mbox
	 * buffer, before the send attempt. If otx2_mbox_wait_for_zero() or
	 * the upstream send fails, that batch is lost with no replay path and
	 * the switch FIB may diverge from the host; tolerating that is a
	 * known limitation for now.
	 */
	while ((l3_entry =
		list_first_entry_or_null(lh,
					 struct l3_entry, list)) != NULL) {
		entry = l3_entry->entry;
		cnt = l3_entry->cnt;

		/* af2swdev_notify_req.entry[] holds RVU_SW_L3_BATCH_MAX slots;
		 * stop before copying the next l3_entry when the mbox buffer
		 * would overflow. Leftovers stay on lh and are re-queued.
		 */
		if (tot_cnt + cnt > RVU_SW_L3_BATCH_MAX)
			break;

		sz = sizeof(*entry) * cnt;

		memcpy(dst, entry, sz);
		for (i = 0; i < cnt; i++)
			dst[i].port_id = l3_entry->port_id;
		tot_cnt += cnt;
		dst += cnt;

		list_del_init(&l3_entry->list);
		kfree(l3_entry);
	}
	if (!tot_cnt) {
		mutex_unlock(&rvu->mbox_lock);
		return -EINVAL;
	}

	req->flags = FIB_CMD;
	req->cnt = tot_cnt;

	rc = otx2_mbox_wait_for_zero(&rvu->afpf_wq_info.mbox_up, swdev_pf);
	if (rc)
		otx2_mbox_msg_send_up(&rvu->afpf_wq_info.mbox_up, swdev_pf);

	mutex_unlock(&rvu->mbox_lock);
	return rc ? 0 : -EFAULT;
}

static void sw_l3_offl_work_handler(struct work_struct *work)
{
	struct list_head l3lh;

	INIT_LIST_HEAD(&l3lh);

	mutex_lock(&l3_offl_llock);
	if (list_empty(&l3_offl_lh)) {
		mutex_unlock(&l3_offl_llock);
		return;
	}
	list_splice_init(&l3_offl_lh, &l3lh);
	mutex_unlock(&l3_offl_llock);

	if (rvu_sw_l3_offl_rule_push(&l3lh))
		pr_err("%s: Error to push rules\n", __func__);

	/* rvu_sw_l3_offl_rule_push() may leave entries when a batch is full. */
	if (!list_empty(&l3lh)) {
		mutex_lock(&l3_offl_llock);
		list_splice(&l3lh, &l3_offl_lh);
		mutex_unlock(&l3_offl_llock);
		if (sw_l3_offl_wq)
			queue_delayed_work(sw_l3_offl_wq, &l3_offl_work,
					   msecs_to_jiffies(100));
		return;
	}

	mutex_lock(&l3_offl_llock);
	if (!list_empty(&l3_offl_lh))
		rvu_sw_l3_queue_work();
	mutex_unlock(&l3_offl_llock);
}

int rvu_mbox_handler_fib_notify(struct rvu *rvu,
				struct fib_notify_req *req,
				struct msg_rsp *rsp)
{
	struct l3_entry *l3_entry;
	int sz, rc;

	if (!(rvu->rswitch.flags & RVU_SWITCH_FLAG_FW_READY))
		return -EAGAIN;

	/* Reject single notifies larger than af2swdev_notify_req.entry[]. */
	if (!req->cnt || req->cnt > RVU_SW_L3_BATCH_MAX)
		return -EINVAL;

	sz = req->cnt * sizeof(struct fib_entry);

	l3_entry = kcalloc(1, sizeof(*l3_entry) + sz, GFP_KERNEL);
	if (!l3_entry)
		return -ENOMEM;

	l3_entry->port_id = rvu_sw_port_id(rvu, req->hdr.pcifunc);
	l3_entry->rvu = rvu;
	l3_entry->cnt = req->cnt;
	INIT_LIST_HEAD(&l3_entry->list);
	memcpy(l3_entry->entry, req->entry, sz);

	/* Host PFs on this RVU share one AF and one switch PF offload path. */
	mutex_lock(&l3_offl_llock);
	rc = rvu_sw_l3_ensure_wq();
	if (rc) {
		mutex_unlock(&l3_offl_llock);
		kfree(l3_entry);
		return rc;
	}

	list_add_tail(&l3_entry->list, &l3_offl_lh);
	if (sw_l3_offl_wq)
		rvu_sw_l3_queue_work();
	mutex_unlock(&l3_offl_llock);

	return 0;
}

void rvu_sw_l3_shutdown(void)
{
	if (!sw_l3_offl_wq)
		return;

	cancel_delayed_work_sync(&l3_offl_work);
	destroy_workqueue(sw_l3_offl_wq);
	sw_l3_offl_wq = NULL;

	mutex_lock(&l3_offl_llock);
	rvu_sw_l3_drain_list(&l3_offl_lh);
	mutex_unlock(&l3_offl_llock);
}
