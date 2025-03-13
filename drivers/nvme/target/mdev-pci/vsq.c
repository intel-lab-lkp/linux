// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtual NVMe submission queue implementation
 * Copyright (c) 2019 - Maxim Levitsky
 * Copyright (C) 2025 Oracle Corporation
 */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "priv.h"

static int nvmet_mdev_calc_vsq_size(struct nvmet_mdev_vsq *vsq)
{
	return round_up(vsq->size * sizeof(struct nvme_command), PAGE_SIZE);
}

/* Delete an virtual completion queue */
void nvmet_mdev_vsq_delete(struct nvmet_mdev_vctrl *vctrl, u16 qid)
{
	struct nvmet_mdev_vsq *q = &vctrl->vsqs[qid];
	bool paused;

	lockdep_assert_held(&vctrl->lock);
	_DBG(vctrl, "VSQ: delete qid=%d\n", q->qid);

	/*
	 * If this is an unclean shutdown make sure we don't try to start
	 * new IOs after we free the queue.
	 */
	paused = nvmet_mdev_io_pause(vctrl);
	clear_bit(qid, vctrl->vsq_en);
	if (paused)
		nvmet_mdev_io_resume(vctrl);

	if (q->nvmet_sq.ctrl)
		nvmet_sq_destroy(&q->nvmet_sq);

	/*
	 * The nvmet sq destruction just waits for the queue_response callout
	 * to return, so we could still have completions queued. Flush
	 * them now since we are freeing the queue below.
	 */
	if (test_and_clear_bit(qid, vctrl->vcq_en)) {
		paused = nvmet_mdev_io_pause(vctrl);
		nvmet_mdev_process_responses(vctrl, q->vcq);
		if (paused)
			nvmet_mdev_io_resume(vctrl);
	}

	kfree(q->reqs);

	nvmet_mdev_udata_queue_vunmap(&vctrl->viommu, q->iova, q->data,
				      q->data_size);
	q->data = NULL;
	q->iova = 0;
}

/* Create new virtual completion queue */
int nvmet_mdev_vsq_init(struct nvmet_mdev_vctrl *vctrl, u16 qid,
			dma_addr_t iova, u16 size, u16 cqid)
{
	struct nvmet_ctrl *ctrl = vctrl->nvmet_ctrl;
	struct nvmet_mdev_vsq *q = &vctrl->vsqs[qid];

	lockdep_assert_held(&vctrl->lock);

	q->vctrl = vctrl;
	q->qid = qid;
	q->size = size;
	q->head = 0;
	q->vcq = &vctrl->vcqs[cqid];
	q->data = NULL;
	q->iova = iova;
	q->data_size = nvmet_mdev_calc_vsq_size(q);

	_DBG(vctrl, "VSQ: create qid=%d depth=%d cqid=%d\n", qid, size, cqid);

	q->data = nvmet_mdev_udata_update_queue_vmap(&vctrl->viommu, q->iova,
						     q->data, q->data_size);
	if (!q->data)
		goto delete;

	q->reqs = kcalloc(size, sizeof(*q->reqs), GFP_KERNEL);
	if (!q->reqs)
		goto delete;

	vctrl->mmio.dbs[q->qid].sqt = 0;
	vctrl->mmio.eidxs[q->qid].sqt = 0;

	if (nvmet_sq_create(ctrl, &q->nvmet_sq, qid, size))
		goto delete;

	set_bit(qid, vctrl->vsq_en);
	return NVME_SC_SUCCESS;

delete:
	nvmet_mdev_vsq_delete(vctrl, qid);
	return NVME_SC_INTERNAL;
}

/* Move queue head one item forward */
static void nvmet_mdev_vsq_advance_head(struct nvmet_mdev_vsq *q)
{
	q->head++;
	if (q->head == q->size)
		q->head = 0;
}

static bool nvmet_mdev_vsq_has_data(struct nvmet_mdev_vctrl *vctrl,
				    struct nvmet_mdev_vsq *q)
{
	u16 tail = le32_to_cpu(READ_ONCE(vctrl->mmio.dbs[q->qid].sqt));

	if (!vctrl->mmio.dbs || !vctrl->mmio.eidxs || !q->data)
		return false;

	if  (tail == q->head)
		return false;

	if (!nvmet_mdev_mmio_db_check(vctrl, q->qid, q->size, tail))
		return false;
	return true;
}

/* get one command from a virtual submission queue */
struct nvme_command *nvmet_mdev_vsq_get_cmd(struct nvmet_mdev_vctrl *vctrl,
					    struct nvmet_mdev_vsq *q,
					    u16 *index)
{
	u16 oldhead = q->head;
	u32 eidx;

	if (!nvmet_mdev_vsq_has_data(vctrl, q))
		return NULL;

	nvmet_mdev_vsq_advance_head(q);

	eidx = q->head + (q->size >> 1);
	if (eidx >= q->size)
		eidx -= q->size;

	WRITE_ONCE(vctrl->mmio.eidxs[q->qid].sqt, cpu_to_le32(eidx));

	*index = oldhead;
	return &q->data[oldhead];
}

bool nvmet_mdev_vsq_suspend_io(struct nvmet_mdev_vsq *q)
{
	struct nvmet_mdev_vctrl *vctrl = q->vctrl;
	u16 tail = le32_to_cpu(vctrl->mmio.dbs[q->qid].sqt);

	/*
	 * If the queue is not in working state don't allow the idle code
	 * to kick in
	 */
	if (!vctrl->mmio.dbs || !vctrl->mmio.eidxs || !q->data)
		return false;

	/* queue has data - refuse idle */
	if (tail != q->head)
		return false;

	/* Write eventid to tell the user to ring normal doorbell */
	vctrl->mmio.eidxs[q->qid].sqt = cpu_to_le32(q->head);

	/* memory barrier to ensure that the user have seen the eidx */
	mb();

	/* Check that doorbell diddn't move meanwhile */
	tail = le32_to_cpu(vctrl->mmio.dbs[q->qid].sqt);
	return (tail == q->head);
}
