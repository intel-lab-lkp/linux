// SPDX-License-Identifier: GPL-2.0+
/*
 * NVMe IO command translation and polling IO thread
 * Copyright (c) 2019 - Maxim Levitsky
 * Copyright (C) 2025 Oracle Corporation
 */
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/nvme.h>
#include <linux/timekeeping.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include "priv.h"

static int nvmet_mdev_create_sgl(struct nvmet_mdev_req *mreq, u64 data_len,
				 const struct nvme_command *cmd)
{
	struct nvmet_ext_data_iter *iter = &mreq->data_iter;
	struct nvmet_req *req = &mreq->req;
	struct scatterlist *sg;
	int i, ret;

	ret = nvmet_mdev_udata_iter_set_dptr(&mreq->data_iter,
					     &cmd->common.dptr, data_len);
	if (ret)
		return ret;

	ret = sg_alloc_table(&mreq->sgt, iter->count, GFP_KERNEL);
	if (ret)
		return ret;

	for_each_sgtable_sg(&mreq->sgt, sg, i) {
		int seg_len = min(PAGE_SIZE, data_len);
		struct page *page;
		int offset;

		page = pfn_to_page(PHYS_PFN(iter->physical));
		offset = offset_in_page(iter->physical);

		sg_set_page(sg, page, seg_len, offset);

		ret = iter->next(iter);
		if (WARN_ON(ret))
			goto release_iter;
		data_len -= seg_len;
	}

	req->sg = mreq->sgt.sgl;
	req->sg_cnt = mreq->sgt.nents;
	return 0;

release_iter:
	if (iter->release)
		iter->release(iter);

	sg_free_table(&mreq->sgt);
	return ret;
}

static bool nvmet_mdev_submit_cmd(struct nvmet_mdev_vctrl *vctrl,
				  struct nvmet_mdev_vsq *vsq)
{
	struct nvmet_mdev_vcq *vcq = vsq->vcq;
	struct nvme_completion cqe = {};
	struct nvmet_mdev_req *mreq;
	struct nvme_command *cmd;
	struct nvmet_req *req;
	u16 ucid;
	int ret;

	cmd = nvmet_mdev_vsq_get_cmd(vctrl, vsq, &ucid);
	if (!cmd)
		return false;

	if (ucid >= vsq->size) {
		ret = DNR(NVME_SC_INVALID_FIELD);
		goto complete;
	}

	if (cmd->common.flags != 0) {
		ret = DNR(NVME_SC_INVALID_FIELD);
		goto complete;
	}

	mreq = &vsq->reqs[ucid];
	memset(mreq, 0, sizeof(*mreq));

	INIT_LIST_HEAD(&mreq->mem_map_list);
	nvmet_mdev_udata_iter_setup(&vctrl->viommu, &mreq->data_iter,
				    &mreq->mem_map_list);
	init_llist_node(&mreq->cq_node);
	mreq->vcq = vcq;

	req = &mreq->req;
	req->cmd = cmd;
	req->cqe = &mreq->cqe;
	req->port = vctrl->nvmet_ctrl->port;

	if (!nvmet_req_init(req, &vcq->nvmet_cq, &vsq->nvmet_sq,
			    &nvmet_mdev_ops)) {
		vctrl->expected_responses++;
		/* nvmet will complete via queue_response */
		return true;
	}

	req->transfer_len = nvmet_req_transfer_len(req);
	if (req->transfer_len) {
		ret = nvmet_mdev_create_sgl(mreq, req->transfer_len, cmd);
		if (ret) {
			ret = nvmet_mdev_translate_error(ret);
			goto uninit_req;
		}
	}

	vctrl->expected_responses++;
	req->execute(req);
	return true;

uninit_req:
	nvmet_req_uninit(req);
complete:
	cqe.sq_head = cpu_to_le16(vsq->head);
	cqe.sq_id = cpu_to_le16(vsq->qid);
	cqe.command_id = cmd->common.command_id;
	cqe.status = cpu_to_le16(ret << 1);

	nvmet_mdev_vcq_write_cqe(vctrl, vcq, &cqe);
	return true;
}

bool nvmet_mdev_process_responses(struct nvmet_mdev_vctrl *vctrl,
				  struct nvmet_mdev_vcq *vcq)
{
	struct nvmet_mdev_req *mreq, *mreq_next;
	struct nvmet_ext_data_iter *iter;
	struct llist_node *node;
	bool processed = false;

	node = llist_del_all(&vcq->mreq_list);
	if (!node)
		return processed;

	llist_for_each_entry_safe(mreq, mreq_next, node, cq_node) {
		iter = &mreq->data_iter;

		nvmet_mdev_vcq_write_cqe(vctrl, vcq, mreq->req.cqe);

		if (iter->release)
			iter->release(iter);

		if (mreq->req.sg_cnt)
			sg_free_table(&mreq->sgt);

		vctrl->expected_responses--;
		processed = true;
	}

	return processed;
}

void nvmet_mdev_io_resume(struct nvmet_mdev_vctrl *vctrl)
{
	if (!vctrl->iothread || !vctrl->iothread_parked || vctrl->io_idle)
		return;

	vctrl->iothread_parked = false;
	/* has memory barrier */
	kthread_unpark(vctrl->iothread);
}

bool nvmet_mdev_io_pause(struct nvmet_mdev_vctrl *vctrl)
{
	if (!vctrl->iothread || vctrl->iothread_parked)
		return false;

	vctrl->iothread_parked = true;
	kthread_park(vctrl->iothread);
	return true;
}

static int nvmet_mdev_get_poll_tmo(struct nvmet_mdev_vctrl *vctrl)
{
	/* can't stop polling when shadow db not enabled */
	return vctrl->mmio.shadow_db_en ? vctrl->poll_timeout_ms : 0;
}

static void nvmet_mdev_process_io(struct nvmet_mdev_vctrl *vctrl)
{
	struct nvmet_mdev_vcq *vcq;
	struct nvmet_mdev_vsq *vsq;
	unsigned long last = jiffies;
	bool idle = false;
	int timeout;
	u16 qid;
	int i;

	vctrl->now = ktime_get();

	/* main loop */
	while (!kthread_should_park()) {
		vctrl->now = ktime_get();

		for_each_set_bit(qid, vctrl->vsq_en, NVMET_MDEV_MAX_NR_QUEUES) {
			vsq = &vctrl->vsqs[qid];

			for (i = 0 ; i < (1 << vctrl->arb_burst_shift) ; i++)
				if (nvmet_mdev_submit_cmd(vctrl, vsq))
					last = jiffies;
		}

		for_each_set_bit(qid, vctrl->vcq_en, NVMET_MDEV_MAX_NR_QUEUES) {
			vcq = &vctrl->vcqs[qid];

			nvmet_mdev_vcq_process(vctrl, vcq, true, vctrl->now);
		}

		for_each_set_bit(qid, vctrl->vcq_en, NVMET_MDEV_MAX_NR_QUEUES) {
			vcq = &vctrl->vcqs[qid];

			if (nvmet_mdev_process_responses(vctrl, vcq))
				last = jiffies;
		}

		/* Check if we need to stop polling */
		timeout = nvmet_mdev_get_poll_tmo(vctrl);
		if (timeout &&
		    time_is_before_jiffies(last + msecs_to_jiffies(timeout))) {
			idle = true;
			break;
		}
		cond_resched();
	}

	for_each_set_bit(qid, vctrl->vcq_en, NVMET_MDEV_MAX_NR_QUEUES) {
		vcq = &vctrl->vcqs[qid];

		/* Drain all the pending completion interrupts to the guest */
		if (nvmet_mdev_vcq_flush(vctrl, vcq, vctrl->now))
			idle = false;
	}

	/*
	 * Park IO thread if IO is truly idle.
	 * TODO - expected_responses will always be > 1 because of async
	 * events.
	 */
	if (!vctrl->expected_responses && idle) {
		if (!mutex_trylock(&vctrl->lock))
			return;

		for_each_set_bit(qid, vctrl->vsq_en, NVMET_MDEV_MAX_NR_QUEUES) {
			vsq = &vctrl->vsqs[qid];

			if (!nvmet_mdev_vsq_suspend_io(vsq))
				idle = false;
		}

		if (idle) {
			_DBG(vctrl, "IO: self-parking\n");
			vctrl->io_idle = true;
			nvmet_mdev_io_pause(vctrl);
		}
		mutex_unlock(&vctrl->lock);
	}
}

static int nvmet_mdev_poll(void *data)
{
	struct nvmet_mdev_vctrl *vctrl = data;

	if (kthread_should_stop())
		return 0;

	_DBG(vctrl, "IO: iothread started\n");

	for (;;) {
		if (kthread_should_park()) {
			_DBG(vctrl, "IO: iothread parked\n");
			kthread_parkme();
		}

		if (kthread_should_stop())
			break;

		nvmet_mdev_process_io(vctrl);
	}

	_DBG(vctrl, "IO: iothread stopped\n");
	return 0;
}

int nvmet_mdev_io_create(struct nvmet_mdev_vctrl *vctrl)
{
	char name[TASK_COMM_LEN];

	_DBG(vctrl, "IO: creating the polling iothread\n");

	snprintf(name, sizeof(name), "nvmet_mdev%d", vctrl->nvmet_ctrl->cntlid);

	vctrl->iothread_parked = false;
	vctrl->io_idle = true;

	vctrl->iothread = kthread_create(nvmet_mdev_poll, vctrl, name);
	if (IS_ERR(vctrl->iothread)) {
		vctrl->iothread = NULL;
		return PTR_ERR(vctrl->iothread);
	}

	if (vctrl->io_idle) {
		vctrl->iothread_parked = true;
		kthread_park(vctrl->iothread);
		return 0;
	}

	wake_up_process(vctrl->iothread);
	return 0;
}

void nvmet_mdev_io_free(struct nvmet_mdev_vctrl *vctrl)
{
	_DBG(vctrl, "IO: destroying the polling iothread\n");
	nvmet_mdev_io_pause(vctrl);
	kthread_stop(vctrl->iothread);
	vctrl->iothread = NULL;
}

void nvmet_mdev_assert_io_not_running(struct nvmet_mdev_vctrl *vctrl)
{
	if (WARN_ON(vctrl->iothread && !vctrl->iothread_parked))
		nvmet_mdev_io_pause(vctrl);
}
