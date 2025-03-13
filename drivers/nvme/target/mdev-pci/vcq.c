// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtual NVMe completion queue implementation
 * Copyright (c) 2019 - Maxim Levitsky
 * Copyright (C) 2025 Oracle Corporation
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "priv.h"

static int nvmet_mdev_calc_vcq_size(struct nvmet_mdev_vcq *vcq)
{
	return round_up(vcq->size * sizeof(struct nvme_completion), PAGE_SIZE);
}

/* Create new virtual completion queue */
int nvmet_mdev_vcq_init(struct nvmet_mdev_vctrl *vctrl, u16 qid,
			dma_addr_t iova, u16 size, int irq)
{
	struct nvmet_ctrl *ctrl = vctrl->nvmet_ctrl;
	struct nvmet_mdev_vcq *q = &vctrl->vcqs[qid];

	lockdep_assert_held(&vctrl->lock);

	q->vctrl = vctrl;
	q->qid = qid;
	q->size = size;
	q->tail = 0;
	q->phase = 1;
	q->irq = irq;
	q->head = 0;
	q->iova = iova;
	q->data = NULL;
	q->data_size = nvmet_mdev_calc_vcq_size(q);
	init_llist_head(&q->mreq_list);

	q->data = nvmet_mdev_udata_update_queue_vmap(&vctrl->viommu, q->iova,
						     (void *)q->data,
						     q->data_size);
	if (!q->data)
		goto delete;

	_DBG(vctrl, "VCQ: create qid=%d depth=%d irq=%d\n", qid, size, irq);

	vctrl->mmio.dbs[q->qid].cqh = 0;
	vctrl->mmio.eidxs[q->qid].cqh = 0;

	if (nvmet_cq_create(ctrl, &q->nvmet_cq, qid, size))
		goto delete;

	set_bit(qid, vctrl->vcq_en);
	return NVME_SC_SUCCESS;

delete:
	nvmet_mdev_vcq_delete(vctrl, qid);
	return NVME_SC_INTERNAL;
}

/* Delete a virtual completion queue */
void nvmet_mdev_vcq_delete(struct nvmet_mdev_vctrl *vctrl, u16 qid)
{
	struct nvmet_mdev_vcq *q = &vctrl->vcqs[qid];

	lockdep_assert_held(&vctrl->lock);

	nvmet_mdev_udata_queue_vunmap(&vctrl->viommu, q->iova, (void *)q->data,
				      q->data_size);
	q->data = NULL;
	q->iova = 0;
	clear_bit(qid, vctrl->vcq_en);

	_DBG(vctrl, "VCQ: delete qid=%d\n", q->qid);
}

/* Move queue tail one item forward */
static void nvmet_mdev_vcq_advance_tail(struct nvmet_mdev_vcq *q)
{
	if (++q->tail == q->size) {
		q->tail = 0;
		q->phase ^= 1;
	}
}

/* Move queue head one item forward */
static void nvmet_mdev_vcq_advance_head(struct nvmet_mdev_vcq *q)
{
	q->head++;
	if (q->head == q->size)
		q->head = 0;
}

/* Process a virtual completion queue */
void nvmet_mdev_vcq_process(struct nvmet_mdev_vctrl *vctrl,
			    struct nvmet_mdev_vcq *q, bool trigger_irqs,
			    ktime_t now)
{
	u16 new_head;
	u32 eidx;

	if (!vctrl->mmio.dbs || !vctrl->mmio.eidxs)
		return;

	new_head = le32_to_cpu(READ_ONCE(vctrl->mmio.dbs[q->qid].cqh));

	if (new_head != q->head) {
		/* bad tail - can't process */
		if (!nvmet_mdev_mmio_db_check(vctrl, q->qid, q->size, new_head))
			return;

		while (q->head != new_head)
			nvmet_mdev_vcq_advance_head(q);

		eidx = q->head + (q->size >> 1);
		if (eidx >= q->size)
			eidx -= q->size;
		WRITE_ONCE(vctrl->mmio.eidxs[q->qid].cqh, cpu_to_le32(eidx));
	}

	if (q->irq != -1 && trigger_irqs) {
		if (q->tail != new_head)
			nvmet_mdev_irq_cond_trigger(vctrl, q->irq, now);
		else
			nvmet_mdev_irq_clear(vctrl, q->irq, now);
	}
}

/* flush interrupts on a completion queue */
bool nvmet_mdev_vcq_flush(struct nvmet_mdev_vctrl *vctrl,
			  struct nvmet_mdev_vcq *q, ktime_t now)
{
	u16 new_head = le32_to_cpu(READ_ONCE(vctrl->mmio.dbs[q->qid].cqh));

	if (new_head == q->tail || q->irq == -1)
		return false;

	nvmet_mdev_irq_trigger(vctrl, q->irq);
	nvmet_mdev_irq_clear(vctrl, q->irq, now);
	return true;
}

void nvmet_mdev_vcq_write_cqe(struct nvmet_mdev_vctrl *vctrl,
			      struct nvmet_mdev_vcq *q,
			      struct nvme_completion *cqe)
{
	volatile u64 *qw = (u64 *)&q->data[q->tail];
	u64 *data = (u64 *)cqe;

	cqe->status = cqe->status | cpu_to_le16(q->phase);

	WRITE_ONCE(qw[0], data[0]);
	/* ensure that hardware sees the phase bit flip last */
	wmb();
	WRITE_ONCE(qw[1], data[1]);

	nvmet_mdev_vcq_advance_tail(q);
	if (q->irq != -1)
		nvmet_mdev_irq_raise(vctrl, q->irq);
}
