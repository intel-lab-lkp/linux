// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/iopoll.h>

#include "mali-vpu-device.h"
#include "mali-vpu-lsid.h"
#include "mali-vpu-registers.h"

#define MALI_VPU_TERMINATE_READ_TIMEOUT_US  10000000
#define MALI_VPU_TERMINATE_POLL_INTERVAL_US 10

static u32 mali_vpu_lsid_reg(u32 lsid, u32 off)
{
	return MALI_VPU_LSID_BASE + (MALI_VPU_LSID_STRIDE * lsid) + off;
}

static u32 mali_vpu_read_lsid(void __iomem *regs_base, u32 lsid, u32 off)
{
	return mali_vpu_read_reg(regs_base, mali_vpu_lsid_reg(lsid, off));
}

static void mali_vpu_write_lsid(void __iomem *regs_base, u32 lsid, u32 off, u32 val)
{
	mali_vpu_write_reg(regs_base, mali_vpu_lsid_reg(lsid, off), val);
}

static bool mali_vpu_is_lsid_allocated(struct mali_vpu_lsid *lsid)
{
	u32 alloc = mali_vpu_read_lsid(lsid->vpu_dev->regs_base, lsid->id,
				       MALI_VPU_REG_LSID_ALLOC);

	return alloc != MALI_VPU_ALLOC_FREE;
}

static u32 mali_vpu_jobqueue_get_lsid(u32 jobqueue, u32 job)
{
	u32 shift = MALI_VPU_JOBQUEUE_JOB_STRIDE * job;
	u32 lsid_mask = (1 << MALI_VPU_JOBQUEUE_LSID_BITS) - 1;

	return (jobqueue >> shift) & lsid_mask;
}

static u32 mali_vpu_jobqueue_get_ncores_minus1(u32 jobqueue, u32 job)
{
	u32 shift = (MALI_VPU_JOBQUEUE_JOB_STRIDE * job) + MALI_VPU_JOBQUEUE_LSID_BITS;
	u32 ncores_mask = (1 << MALI_VPU_JOBQUEUE_NCORES_BITS) - 1;

	return (jobqueue >> shift) & ncores_mask;
}

static u32 mali_vpu_jobqueue_set_job(u32 jobqueue, u32 job_id, u32 lsid,
				     u32 ncores_minus1)
{
	u32 shift = MALI_VPU_JOBQUEUE_JOB_STRIDE * job_id;
	u32 job_mask = ((1u << MALI_VPU_JOBQUEUE_JOB_STRIDE) - 1) << shift;
	u32 job_val, lsid_mask, ncores_mask;

	lsid_mask = ((1u << MALI_VPU_JOBQUEUE_LSID_BITS) - 1);
	ncores_mask = ((1u << MALI_VPU_JOBQUEUE_NCORES_BITS) - 1);

	job_val = lsid & lsid_mask;
	job_val |= (ncores_minus1 & ncores_mask) << MALI_VPU_JOBQUEUE_LSID_BITS;

	return (jobqueue & ~job_mask) | (job_val << shift);
}

int mali_vpu_lsid_terminate(struct mali_vpu_lsid *lsid)
{
	struct mali_vpu_dev *vpu_dev = lsid->vpu_dev;
	u32 val;

	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_TERMINATE, 1);

	return read_poll_timeout(mali_vpu_read_lsid, val, val == 0,
				MALI_VPU_TERMINATE_POLL_INTERVAL_US,
				MALI_VPU_TERMINATE_READ_TIMEOUT_US, false,
				vpu_dev->regs_base, lsid->id,
				MALI_VPU_REG_LSID_TERMINATE);
}

void mali_vpu_lsid_init(struct mali_vpu_lsid *lsid, struct mali_vpu_dev *dev, u32 id)
{
	lsid->vpu_dev = dev;
	lsid->id = id;
}

int mali_vpu_lsid_map(struct mali_vpu_lsid *lsid, struct mali_vpu_lsid_pcb *pcb)
{
	struct mali_vpu_dev *vpu_dev = lsid->vpu_dev;
	u32 alloc;
	int ret;

	/* Check that the LSID is not already allocated. */
	if (mali_vpu_is_lsid_allocated(lsid)) {
		dev_err(vpu_dev->dev,
			"failed to map session to LSID. LSID already allocated. lsid=%u\n",
			lsid->id);
		return -EBUSY;
	}

	/* Allocate LSID. */
	alloc = pcb->nprot == 0 ? MALI_VPU_ALLOC_PROTECTED : MALI_VPU_ALLOC_NON_PROTECTED;
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_ALLOC, alloc);

	if (mali_vpu_read_lsid(vpu_dev->regs_base, lsid->id,
			       MALI_VPU_REG_LSID_NPROT) != pcb->nprot) {
		dev_err(vpu_dev->dev,
			"unexpected protection state for LSID %u\n", lsid->id);
		mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id,
				    MALI_VPU_REG_LSID_ALLOC, MALI_VPU_ALLOC_FREE);
		return -EIO;
	}

	/* Ensure that the cores have stopped executing any session previously mapped. */
	ret = mali_vpu_lsid_terminate(lsid);
	if (ret) {
		dev_err(vpu_dev->dev, "failed to terminate LSID %u: %d\n",
			lsid->id, ret);
		/*
		 * The caller must reset the VPU on termination timeout. The
		 * LSID cannot safely be marked free while hardware activity
		 * is uncertain.
		 */
		return ret;
	}

	/* Configure number of cores to use and which cores to disable. */
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_CTRL, pcb->ctrl);

	/* Configure MMU L0 entry and flush MMU tables. */
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_MMU_CTRL,
			    pcb->mmu_ctrl);
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_FLUSH_ALL, 0);

	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_BUSATTR_0, 0x0);
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_BUSATTR_1, 0x0);
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_BUSATTR_2, 0x33);
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_BUSATTR_3, 0x33);

	/* Restore interrupt registers. */
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_IRQVE, 0);
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_IRQHOST, pcb->irqhost);

	/*
	 * Order the configuration writes before enabling scheduling.
	 */
	wmb();

	/* Enable LSID for scheduling. */
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_SCHED, 1);

	return 0;
}

/*
 * An LSID must not be unmapped if it is present in the job queue or core LSID.
 * It is the responsibility of the scheduler to guarantee that the LSID is idle
 * before it is unmapped.
 */
void mali_vpu_lsid_unmap(struct mali_vpu_lsid *lsid, struct mali_vpu_lsid_pcb *pcb)
{
	struct mali_vpu_dev *vpu_dev = lsid->vpu_dev;

	if (!mali_vpu_is_lsid_allocated(lsid)) {
		dev_dbg(vpu_dev->dev, "no session mapped to LSID %u\n", lsid->id);
		return;
	}

	/* Prevent the LSID from being scheduled. */
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_SCHED, 0);

	/* Store registers in process control block. */
	pcb->ctrl = mali_vpu_read_lsid(vpu_dev->regs_base, lsid->id,
				       MALI_VPU_REG_LSID_CTRL);
	pcb->mmu_ctrl = mali_vpu_read_lsid(vpu_dev->regs_base, lsid->id,
					   MALI_VPU_REG_LSID_MMU_CTRL);
	pcb->irqhost = mali_vpu_read_lsid(vpu_dev->regs_base, lsid->id,
					  MALI_VPU_REG_LSID_IRQHOST);
	pcb->nprot = mali_vpu_read_lsid(vpu_dev->regs_base, lsid->id,
					MALI_VPU_REG_LSID_NPROT);

	/* Deallocate LSID. */
	mali_vpu_write_lsid(vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_ALLOC,
			    MALI_VPU_ALLOC_FREE);
}

int mali_vpu_lsid_jobqueue_add(struct mali_vpu_lsid *lsid, u32 ncores)
{
	struct mali_vpu_dev *vpu_dev = lsid->vpu_dev;
	u32 jobqueue;
	int job, ret = -EAGAIN;

	if (!ncores || ncores > BIT(MALI_VPU_JOBQUEUE_NCORES_BITS))
		return -EINVAL;

	/* Stop the hardware scheduler while updating the jobqueue. */
	mali_vpu_write_reg(vpu_dev->regs_base, MALI_VPU_REG_ENABLE, 0);

	jobqueue = mali_vpu_read_reg(vpu_dev->regs_base, MALI_VPU_REG_JOBQUEUE);

	/* Search if the LSID is already in the job queue. */
	for (job = 0; job < MALI_VPU_JOBQUEUE_NJOBS; job++)
		if (mali_vpu_jobqueue_get_lsid(jobqueue, job) == lsid->id) {
			ret = 0;
			goto enable_hw_scheduler;
		}

	/* Search for a free slot in the job queue. */
	for (job = 0; job < MALI_VPU_JOBQUEUE_NJOBS; job++) {
		if (mali_vpu_jobqueue_get_lsid(jobqueue, job) != MALI_VPU_JOBQUEUE_JOB_INVALID)
			continue;

		jobqueue = mali_vpu_jobqueue_set_job(jobqueue, job, lsid->id, ncores - 1);
		mali_vpu_write_reg(vpu_dev->regs_base, MALI_VPU_REG_JOBQUEUE, jobqueue);
		ret = 0;
		break;
	}

enable_hw_scheduler:
	mali_vpu_write_reg(vpu_dev->regs_base, MALI_VPU_REG_ENABLE, 1);
	return ret;
}

void mali_vpu_lsid_jobqueue_remove(struct mali_vpu_lsid *lsid)
{
	struct mali_vpu_dev *vpu_dev = lsid->vpu_dev;
	u32 old_jobqueue, new_jobqueue = 0;
	u32 old_job_id, new_job_id, old_lsid, old_ncores_minus1;

	/* Disable scheduling. */
	mali_vpu_write_reg(vpu_dev->regs_base, MALI_VPU_REG_ENABLE, 0);

	old_jobqueue = mali_vpu_read_reg(vpu_dev->regs_base, MALI_VPU_REG_JOBQUEUE);

	/* Copy job entries that do not match the LSID to be removed. */
	for (old_job_id = 0, new_job_id = 0; old_job_id < MALI_VPU_JOBQUEUE_NJOBS; old_job_id++) {
		old_lsid          = mali_vpu_jobqueue_get_lsid(old_jobqueue, old_job_id);
		old_ncores_minus1 = mali_vpu_jobqueue_get_ncores_minus1(old_jobqueue, old_job_id);

		if (old_lsid == MALI_VPU_JOBQUEUE_JOB_INVALID ||
		    old_lsid == lsid->id)
			continue;

		new_jobqueue = mali_vpu_jobqueue_set_job(new_jobqueue, new_job_id,
							 old_lsid,
							 old_ncores_minus1);
		new_job_id++;
	}

	while (new_job_id < MALI_VPU_JOBQUEUE_NJOBS) {
		new_jobqueue = mali_vpu_jobqueue_set_job(new_jobqueue, new_job_id,
							 MALI_VPU_JOBQUEUE_JOB_INVALID, 0);
		new_job_id++;
	}

	mali_vpu_write_reg(vpu_dev->regs_base, MALI_VPU_REG_JOBQUEUE, new_jobqueue);
	mali_vpu_write_reg(vpu_dev->regs_base, MALI_VPU_REG_ENABLE, 1);
}

void mali_vpu_lsid_send_irq(struct mali_vpu_lsid *lsid)
{
	mali_vpu_write_lsid(lsid->vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_IRQHOST, 1);
}

void mali_vpu_lsid_flush_mmu(struct mali_vpu_lsid *lsid)
{
	mali_vpu_write_lsid(lsid->vpu_dev->regs_base, lsid->id, MALI_VPU_REG_LSID_FLUSH_ALL, 0);
}
