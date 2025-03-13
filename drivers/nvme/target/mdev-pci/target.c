// SPDX-License-Identifier: GPL-2.0+
/*
 * NVMe target callouts
 * Copyright (c) 2019 - Maxim Levitsky
 * Copyright (C) 2025 Oracle Corporation
 */
#include <linux/device.h>
#include <linux/nvme.h>
#include <linux/module.h>
#include <linux/mdev.h>
#include <linux/slab.h>
#include "../nvmet.h"
#include "../../host/nvme.h"
#include "../../host/fabrics.h"
#include "priv.h"

static void nvmet_mdev_delete_ctrl(struct nvmet_ctrl *ctrl)
{
	struct nvmet_mdev_port *mport = ctrl->port->priv;

	mutex_lock(&mport->mutex);
	nvmet_mdev_remove_ctrl(ctrl->drvdata);
	mutex_unlock(&mport->mutex);
}

static void nvmet_mdev_remove_port(struct nvmet_port *port)
{
	nvmet_mdev_unregister_port(port->priv);
}

static int nvmet_mdev_count_ctrls(void *priv, struct nvmet_port *port,
				  struct nvmet_ctrl *ctrl)
{
	int *count = priv;

	(*count)++;
	return 0;
}

static int nvmet_mdev_add_port(struct nvmet_port *port)
{
	struct nvmet_mdev_port *mport;
	int count = 0;
	int ret;

	ret = nvmet_for_each_static_ctrl(port, nvmet_mdev_ops.type,
					 nvmet_mdev_count_ctrls, &count);
	if (ret)
		return ret;

	if (!count) {
		pr_err("Controllers must be added and enabled before enabling port.\n");
		return -ENODEV;
	}

	mport = kzalloc(sizeof(*mport), GFP_KERNEL);
	if (!mport)
		return -ENOMEM;

	mutex_init(&mport->mutex);
	port->priv = mport;
	mport->nvmet_port = port;
	mport->ctrl_count = count;

	return nvmet_mdev_register_port(mport);
}

static u16 nvmet_mdev_adm_create_cq(struct nvmet_ctrl *ctrl, u16 cqid,
				    u16 cq_flags, u16 qsize, u64 prp1, u16 irq)
{
	struct nvmet_mdev_vctrl *vctrl = ctrl->drvdata;
	u16 ret;

	mutex_lock(&vctrl->lock);
	if (cqid >= NVMET_MDEV_MAX_NR_QUEUES || test_bit(cqid, vctrl->vcq_en)) {
		ret = DNR(NVME_SC_QID_INVALID);
		goto unlock;
	}

	if (!(cq_flags & NVME_QUEUE_PHYS_CONTIG)) {
		ret = DNR(NVME_SC_INVALID_QUEUE);
		goto unlock;
	}

	if (cq_flags & NVME_CQ_IRQ_ENABLED) {
		if (irq >= MAX_VIRTUAL_IRQS) {
			ret = DNR(NVME_SC_INVALID_VECTOR);
			goto unlock;
		}
	}

	ret = nvmet_mdev_vcq_init(vctrl, cqid, prp1, qsize + 1, irq);
unlock:
	mutex_unlock(&vctrl->lock);
	return ret;
}

static u16 nvmet_mdev_adm_delete_cq(struct nvmet_ctrl *ctrl, u16 cqid)
{
	struct nvmet_mdev_vctrl *vctrl = ctrl->drvdata;
	u16 ret = NVME_SC_SUCCESS;

	mutex_lock(&vctrl->lock);
	if (cqid >= NVMET_MDEV_MAX_NR_QUEUES ||
	    !test_bit(cqid, vctrl->vcq_en)) {
		ret = DNR(NVME_SC_QID_INVALID);
		goto unlock;
	}

	nvmet_mdev_vcq_delete(vctrl, cqid);
unlock:
	mutex_unlock(&vctrl->lock);
	return ret;
}

static u16 nvmet_mdev_adm_create_sq(struct nvmet_ctrl *ctrl, u16 sqid,
				    u16 sq_flags, u16 qsize, u64 prp1)
{
	struct nvmet_mdev_vctrl *vctrl = ctrl->drvdata;
	u16 ret;

	mutex_lock(&vctrl->lock);
	if (sqid >= NVMET_MDEV_MAX_NR_QUEUES || test_bit(sqid, vctrl->vsq_en)) {
		ret = DNR(NVME_SC_QID_INVALID);
		goto unlock;
	}

	/*
	 * sqid and cqid are checked they are equal by nvmet before calling
	 * this
	 */
	if (!test_bit(sqid, vctrl->vcq_en)) {
		ret = DNR(NVME_SC_CQ_INVALID);
		goto unlock;
	}

	if (!(sq_flags & NVME_QUEUE_PHYS_CONTIG)) {
		ret = DNR(NVME_SC_INVALID_QUEUE);
		goto unlock;
	}

	ret = nvmet_mdev_vsq_init(vctrl, sqid, prp1, qsize + 1, sqid);
unlock:
	mutex_unlock(&vctrl->lock);
	return ret;
}

static u16 nvmet_mdev_adm_delete_sq(struct nvmet_ctrl *ctrl, u16 sqid)
{
	struct nvmet_mdev_vctrl *vctrl = ctrl->drvdata;
	u16 ret = NVME_SC_SUCCESS;

	mutex_lock(&vctrl->lock);
	if (sqid >= NVMET_MDEV_MAX_NR_QUEUES ||
	    !test_bit(sqid, vctrl->vsq_en)) {
		ret = DNR(NVME_SC_QID_INVALID);
		goto unlock;
	}

	nvmet_mdev_vsq_delete(vctrl, sqid);
unlock:
	mutex_unlock(&vctrl->lock);
	return ret;
}

static u16 nvmet_mdev_adm_get_features(const struct nvmet_ctrl *ctrl, u8 feat,
				       void *feat_data)
{
	struct nvmet_mdev_vctrl *vctrl = ctrl->drvdata;
	struct nvmet_feat_arbitration *arb;
	struct nvmet_feat_irq_coalesce *irqc;
	struct nvmet_feat_irq_config *irqcfg;

	switch (feat) {
	case NVME_FEAT_ARBITRATION:
		arb = feat_data;

		arb->ab = vctrl->arb_burst_shift;
		break;
	case NVME_FEAT_IRQ_COALESCE:
		irqc = feat_data;

		irqc->thr = vctrl->irqs.irq_coalesc_max - 1;

		irqc->time = vctrl->irqs.irq_coalesc_time_us;
		do_div(irqc->time, 100);
		break;
	case NVME_FEAT_IRQ_CONFIG:
		irqcfg = feat_data;

		if (irqcfg->iv >= MAX_VIRTUAL_IRQS)
			return DNR(NVME_SC_INVALID_FIELD);

		irqcfg->cd = vctrl->irqs.vecs[irqcfg->iv].irq_coalesc_en;
		break;
	default:
		return DNR(NVME_SC_INVALID_FIELD);
	}

	return NVME_SC_SUCCESS;
}

static u16 nvmet_mdev_adm_set_features(const struct nvmet_ctrl *ctrl, u8 feat,
				       void *feat_data)
{
	struct nvmet_mdev_vctrl *vctrl = ctrl->drvdata;
	struct nvmet_feat_arbitration *arb;
	struct nvmet_feat_irq_coalesce *irqc;
	struct nvmet_feat_irq_config *irqcfg;

	switch (feat) {
	case NVME_FEAT_ARBITRATION:
		arb = feat_data;

		vctrl->arb_burst_shift = arb->ab;
		break;
	case NVME_FEAT_IRQ_COALESCE:
		irqc = feat_data;

		vctrl->irqs.irq_coalesc_max = irqc->thr + 1;
		vctrl->irqs.irq_coalesc_time_us = irqc->time * 100;
		break;
	case NVME_FEAT_IRQ_CONFIG:
		irqcfg = feat_data;

		if (irqcfg->iv >= MAX_VIRTUAL_IRQS)
			return DNR(NVME_SC_INVALID_FIELD);

		vctrl->irqs.vecs[irqcfg->iv].irq_coalesc_en = irqcfg->cd != 0;
		break;
	default:
		return DNR(NVME_SC_INVALID_FIELD);
	}

	return NVME_SC_SUCCESS;
}

static u16 nvmet_mdev_adm_set_dbbuf(struct nvmet_ctrl *ctrl, u64 prp1, u64 prp2)
{
	struct nvmet_mdev_vctrl *vctrl = ctrl->drvdata;
	int ret;

	if (!vctrl->mmio.shadow_db_supported)
		return DNR(NVME_SC_INVALID_OPCODE);

	if (vctrl->mmio.shadow_db_en)
		return DNR(NVME_SC_INVALID_FIELD);

	if ((offset_in_page(prp1) != 0) || (offset_in_page(prp2) != 0))
		return DNR(NVME_SC_INVALID_FIELD);

	ret = nvmet_mdev_mmio_enable_dbs_shadow(vctrl, prp1, prp2);
	return nvmet_mdev_translate_error(ret);
}

static void nvmet_mdev_queue_response(struct nvmet_req *req)
{
	struct nvmet_mdev_req *mreq = container_of(req, struct nvmet_mdev_req,
						   req);

	llist_add(&mreq->cq_node, &mreq->vcq->mreq_list);
}

const struct nvmet_fabrics_ops nvmet_mdev_ops = {
	.owner			= THIS_MODULE,
	.flags			= NVMF_SGLS_NOT_SUPP | NVMF_STATIC_CTRL,
	.type			= NVMF_TRTYPE_MDEV_PCI,
	.delete_ctrl		= nvmet_mdev_delete_ctrl,
	.add_port		= nvmet_mdev_add_port,
	.remove_port		= nvmet_mdev_remove_port,
	.create_cq		= nvmet_mdev_adm_create_cq,
	.delete_cq		= nvmet_mdev_adm_delete_cq,
	.create_sq		= nvmet_mdev_adm_create_sq,
	.delete_sq		= nvmet_mdev_adm_delete_sq,
	.set_feature		= nvmet_mdev_adm_set_features,
	.get_feature		= nvmet_mdev_adm_get_features,
	.set_dbbuf		= nvmet_mdev_adm_set_dbbuf,
	.queue_response		= nvmet_mdev_queue_response,
};

MODULE_AUTHOR("Maxim Levitsky <mlevitsk@redhat.com>, "
	      "Mike Christie <michael.christie@oracle.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("nvmet-transport-253"); /* 253 == NVMF_TRTYPE_MDEV_PCI */
