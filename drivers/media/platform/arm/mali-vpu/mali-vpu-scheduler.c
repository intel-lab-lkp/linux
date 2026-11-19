// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/bitfield.h>
#include <linux/string.h>

#include "mali-vpu-device.h"
#include "mali-vpu-mmu.h"
#include "mali-vpu-registers.h"
#include "mali-vpu-scheduler.h"
#include "mali-vpu-session.h"
#include "mali-vpu-variant.h"

int mali_vpu_sched_init(struct mali_vpu_sched *sched, struct mali_vpu_dev *vpu_dev)
{
	u32 hw_nlsid, variant_nlsid;
	u32 lsid, max_irq_lsid;

	if (!vpu_dev->variant || !vpu_dev->variant->max_lsid)
		return -EINVAL;

	hw_nlsid = mali_vpu_read_reg(vpu_dev->regs_base, MALI_VPU_REG_NLSID);
	variant_nlsid = vpu_dev->variant->max_lsid;

	max_irq_lsid = min_t(u32, BITS_PER_LONG, MALI_VPU_IRQVE_NUM_BITS);
	if (variant_nlsid > max_irq_lsid) {
		dev_err(vpu_dev->dev,
			"variant supports %u LSIDs, but IRQ handling supports only %u\n",
			variant_nlsid, max_irq_lsid);
		return -EINVAL;
	}

	if (!hw_nlsid || hw_nlsid > variant_nlsid) {
		dev_err(vpu_dev->dev,
			"hardware reports %u LSIDs, but variant supports only %u\n",
			hw_nlsid, variant_nlsid);
		return -EINVAL;
	}

	sched->nlsid = hw_nlsid;
	sched->lsid_list = devm_kcalloc(vpu_dev->dev, sched->nlsid,
					sizeof(*sched->lsid_list), GFP_KERNEL);
	if (!sched->lsid_list)
		return -ENOMEM;

	sched->vpu_dev = vpu_dev;
	mutex_init(&sched->lock);

	for (lsid = 0; lsid < sched->nlsid; lsid++)
		mali_vpu_lsid_init(&sched->lsid_list[lsid], vpu_dev, lsid);

	mali_vpu_write_reg(vpu_dev->regs_base, MALI_VPU_REG_RESET, 1);
	mali_vpu_write_reg(vpu_dev->regs_base, MALI_VPU_REG_CLKFORCE, 0);

	return 0;
}

void mali_vpu_sched_cleanup(struct mali_vpu_sched *sched)
{
	mutex_destroy(&sched->lock);
}

int mali_vpu_sched_session_prepare(struct mali_vpu_sched_session *sched_sess,
				   struct mali_vpu_session *sess)
{
	u32 hw_ncores, ctrl = 0;

	if (sched_sess->lsid)
		return -EBUSY;

	if (!sess->mmu.root.cpu)
		return -EINVAL;

	/* Simple scheduling policy - all cores for all sessions */
	hw_ncores = mali_vpu_read_reg(sess->vpu_dev->regs_base, MALI_VPU_REG_NCORES);
	if (!hw_ncores ||
	    hw_ncores > BIT(MALI_VPU_JOBQUEUE_NCORES_BITS))
		return -EINVAL;

	ctrl |= FIELD_PREP(MALI_VPU_CTRL_MAXCORES, hw_ncores);

	memset(sched_sess, 0, sizeof(*sched_sess));

	sched_sess->core_count = hw_ncores;
	sched_sess->pcb.ctrl = ctrl;
	sched_sess->pcb.mmu_ctrl = mali_vpu_mmu_get_root_pte(&sess->mmu);
	sched_sess->pcb.nprot = 1;

	sess->ncores = sched_sess->core_count;

	return 0;
}

static void mali_vpu_sched_reset_locked(struct mali_vpu_sched *sched)
{
	struct mali_vpu_lsid *lsid;
	u32 i;

	lockdep_assert_held(&sched->lock);

	mali_vpu_write_reg(sched->vpu_dev->regs_base, MALI_VPU_REG_RESET, 1);

	for (i = 0; i < sched->nlsid; i++) {
		lsid = &sched->lsid_list[i];

		if (lsid->sched_sess)
			lsid->sched_sess->lsid = NULL;

		lsid->sched_sess = NULL;
	}

	mali_vpu_write_reg(sched->vpu_dev->regs_base, MALI_VPU_REG_CLKFORCE, 0);
}

int mali_vpu_sched_switch_in(struct mali_vpu_sched_session *sched_sess,
			     struct mali_vpu_sched *sched)
{
	struct mali_vpu_lsid *lsid;
	struct mali_vpu_sched_session *owner;
	bool mapped_here = false;
	int ret = 0;

	mutex_lock(&sched->lock);

	if (!sched->nlsid) {
		ret = -ENODEV;
		goto err_unlock;
	}

	/* Allow only one active session as a simple policy */
	lsid = &sched->lsid_list[0];

	owner = lsid->sched_sess;
	if (owner && owner != sched_sess) {
		ret = -EBUSY;
		goto err_unlock;
	}

	if (sched_sess->lsid && sched_sess->lsid != lsid) {
		ret = -EINVAL;
		goto err_unlock;
	}

	if (!sched_sess->lsid) {
		/* Map the session to an lsid first */
		ret = mali_vpu_lsid_map(lsid, &sched_sess->pcb);
		if (ret) {
			if (ret == -ETIMEDOUT)
				mali_vpu_sched_reset_locked(sched);
			goto err_unlock;
		}

		lsid->sched_sess = sched_sess;
		sched_sess->lsid = lsid;
		mapped_here = true;
	}

	/* Add the session to jobqueue */
	ret = mali_vpu_lsid_jobqueue_add(sched_sess->lsid, sched_sess->core_count);
	if (ret && mapped_here) {
		mali_vpu_lsid_unmap(lsid, &sched_sess->pcb);
		lsid->sched_sess = NULL;
		sched_sess->lsid = NULL;
	}

err_unlock:
	mutex_unlock(&sched->lock);
	return ret;
}

void mali_vpu_sched_send_irq(struct mali_vpu_sched_session *sched_sess,
			     struct mali_vpu_sched *sched)
{
	mutex_lock(&sched->lock);
	if (!sched_sess->lsid)
		sched_sess->pcb.irqhost = 1;
	else
		mali_vpu_lsid_send_irq(sched_sess->lsid);
	mutex_unlock(&sched->lock);
}

void mali_vpu_sched_flush_mmu(struct mali_vpu_sched_session *sched_sess,
			      struct mali_vpu_sched *sched)
{
	mutex_lock(&sched->lock);
	if (sched_sess->lsid)
		mali_vpu_lsid_flush_mmu(sched_sess->lsid);
	mutex_unlock(&sched->lock);
}

int mali_vpu_sched_session_terminate(struct mali_vpu_sched_session *sched_sess,
				     struct mali_vpu_sched *sched)
{
	struct mali_vpu_lsid *lsid;
	int ret = 0;

	mutex_lock(&sched->lock);

	lsid = sched_sess->lsid;
	if (!lsid)
		goto unlock;

	mali_vpu_lsid_jobqueue_remove(sched_sess->lsid);

	ret = mali_vpu_lsid_terminate(sched_sess->lsid);
	if (ret) {
		dev_err(sched->vpu_dev->dev,
			"failed to terminate LSID %u: %d; resetting VPU\n",
			lsid->id, ret);
		mali_vpu_sched_reset_locked(sched);
		goto unlock;
	}

	mali_vpu_lsid_unmap(lsid, &sched_sess->pcb);
	lsid->sched_sess = NULL;
	sched_sess->lsid = NULL;

unlock:
	mutex_unlock(&sched->lock);

	return ret;
}
