// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arm CLA driver - save/restore accelerator context
 *
 * Copyright 2026 Arm Limited.
 */

#include <linux/io.h>

#include "arm-cla.h"

static int cla_regs_save_accel(struct cla_dev *dev, unsigned int accid,
			       struct cla_regs *regs, off_t *regstate_off)
{
	int ret;
	u64 status;
	size_t regstate_size;
	u64 *srstate = regs->srstate[accid];

	ret = cla_op_entersr(dev, accid, srstate);
	if (ret) {
		/*
		 * Note that we don't expect the accelerator to return an error.
		 * Implementations that don't support context switch cancel the
		 * work and write an error in SRSTATE_STATUS, but don't fail
		 * ENTERSR.
		 */
		return ret;
	}

	status = cla_reg_read(dev, CLA_REG_STATUS(accid));
	if ((status & CLA_STATUS_STATE_MASK) != CLA_STATUS_STATE_SRMODE) {
		cla_err(dev, "unexpected SR status 0x%llx\n", status);
		return -EIO;
	}

	/*
	 * A device that supports SROP fails device probe. Others should not
	 * report SROP here, we don't support it.
	 */
	WARN_ON(FIELD_GET(CLA_SRSTATE_0_SROP, srstate[0]));

	regstate_size = FIELD_GET(CLA_SRSTATE_0_REGSTATE, srstate[0]);
	if (WARN_ON(*regstate_off + regstate_size * 8 > dev->iassizes))
		return -ENOSPC;

	if (regstate_size) {
		ret = cla_op_regread(dev, accid, CLA_REG_IASn, regstate_size,
				     regs->regstate + *regstate_off);
		if (ret) {
			cla_err(dev, "failed to save regstate: %d\n", ret);
			return ret;
		}
	}

	*regstate_off += regstate_size;

	return 0;
}

/*
 * Errors are very unlikely, but if they happen the device is left in SRMODE.
 */
static int cla_regs_restore_accel(struct cla_dev *dev, unsigned int accid,
				  struct cla_regs *regs, off_t *regstate_off)
{
	int ret;
	u64 status;
	size_t regstate_size;
	u64 *srstate = regs->srstate[accid];

	/*
	 * The accelerator was reset and isn't in SRMODE. Later we could support
	 * coming directly from cla_regs_save_accel() in SRMODE, but at the
	 * moment we always need a RESET.
	 */
	status = cla_reg_read(dev, CLA_REG_STATUS(accid));
	if ((status & CLA_STATUS_STATE_MASK) != CLA_STATUS_STATE_IDLE) {
		cla_err(dev, "unexpected status 0x%llx\n", status);
		return -EIO;
	}

	ret = cla_op_entersr(dev, accid, NULL);
	if (ret)
		return ret;

	WARN_ON(FIELD_GET(CLA_SRSTATE_0_SROP, srstate[0]));

	regstate_size = FIELD_GET(CLA_SRSTATE_0_REGSTATE, srstate[0]);
	if (WARN_ON(*regstate_off + regstate_size * 8 > dev->iassizes))
		return -ENOSPC;

	if (regstate_size) {
		ret = cla_op_regwrite(dev, accid, CLA_REG_IASn, regstate_size,
				      regs->regstate + *regstate_off);
		if (ret) {
			cla_err(dev, "failed to restore regstate: %d\n", ret);
			return ret;
		}
	}

	*regstate_off += regstate_size;

	return cla_op_exitsr(dev, accid, srstate);
}

/**
 * cla_regs_switch_out - Save CLA context and reset all accelerators
 * @dev: CLA device
 * @regs: CLA register state to save
 * @save_regs: whether to save the DATA and accelerator state
 *
 * When this completes, all accelerators are idle. If this fails, some
 * accelerators may still be running.
 *
 * Return: 0 on success, or an error
 */
int cla_regs_switch_out(struct cla_dev *dev, struct cla_regs *regs,
			bool save_regs)
{
	int i;
	int ret;
	unsigned int accid;
	off_t regstate_off = 0;

	/*
	 * When we interrupt the user process in the middle of launching a
	 * command, we have to wait for LRESP_PENDING to clear before we can
	 * launch a new command or save the DATA registers.
	 */
	ret = cla_op_wait_lresp(dev, &regs->lresp);
	if (ret)
		return ret;

	if (save_regs) {
		for (i = 0; i < CLA_NUM_DATA_REGS; i++)
			regs->data[i] = cla_reg_read(dev, CLA_REG_DATA(i));

		cla_for_each_accid(dev, accid) {
			ret = cla_regs_save_accel(dev, accid, regs, &regstate_off);
			if (ret)
				return ret;
		}

		regs->accel_valid = true;
	}

	/*
	 * "If the accelerator is non idle when ENTERSR is launched, then a
	 * RESET operation is required after the internal accelerator state has
	 * been saved"
	 *
	 * However some accelerators keep stale internal state and caches even
	 * after job completion, so always RESET for now.
	 */
	return cla_op_reset_all(dev);
}

/**
 * cla_regs_switch_in - Restore CLA context
 * @dev: CLA device
 * @regs: CLA register state to restore
 *
 * Restore the DATA and LRESP registers, and accelerator state if one has been
 * saved. This function is called with all accelerators idle and no trace of
 * previous work.
 *
 * Return: 0 on success, or an error
 */
int cla_regs_switch_in(struct cla_dev *dev, struct cla_regs *regs)
{
	int i;
	int ret;
	unsigned int accid;
	off_t regstate_off = 0;

	cla_for_each_accid(dev, accid) {
		if (regs->accel_valid) {
			ret = cla_regs_restore_accel(dev, accid, regs, &regstate_off);
			if (ret)
				return ret;
			/*
			 * At this point we must not read STATUS because that
			 * would clear EVENT. Userspace always gets a spurious
			 * EVENT on restore, because we have no way to
			 * save/restore EVENT and userspace missing an event
			 * would be worse than getting a spurious one.
			 */
		} else {
			/*
			 * If this context has never been scheduled, then we
			 * just clean the CLA regs. Also clear EVENT by reading
			 * STATUS, to provide a pristine context.
			 */
			cla_reg_read(dev, CLA_REG_STATUS(accid));
		}
	}

	for (i = 0; i < CLA_NUM_DATA_REGS; i++)
		cla_reg_write(dev, CLA_REG_DATA(i), regs->data[i]);
	cla_reg_write(dev, CLA_REG_LRESP, regs->lresp);

	return 0;
}

/**
 * cla_regs_alloc_domain - Allocate register state for a CLA domain
 * @domain: CLA domain
 *
 * Allocate register state to save and restore every device in @domain.
 *
 * Return: an array of register state pointers on success, %NULL on failure.
 */
struct cla_regs **cla_regs_alloc_domain(struct cla_domain *domain)
{
	int i;
	size_t size;
	struct cla_regs **regs_ptrs;

	regs_ptrs = kmalloc_objs(*regs_ptrs, domain->nr_devs,
				 GFP_KERNEL_ACCOUNT);
	if (!regs_ptrs)
		return NULL;

	for (i = 0; i < domain->nr_devs; i++) {
		struct cla_regs *regs;
		struct cla_dev *dev = domain->devs[i];

		/*
		 * The regs structures are only ever accessed from the CLA
		 * device's CPU, so try to allocate them on the right NUMA node
		 */
		size = sizeof(*regs) + dev->iassizes;
		regs = kvzalloc_node(size, GFP_KERNEL_ACCOUNT,
				     cpu_to_node(dev->cpu));
		if (!regs)
			goto err_free;

		regs_ptrs[i] = regs;
	}

	return regs_ptrs;

err_free:
	for (i--; i >= 0; i--)
		kvfree(regs_ptrs[i]);
	kfree(regs_ptrs);
	return NULL;
}

/**
 * cla_regs_free_domain - Free register state for a CLA domain
 * @domain: CLA domain
 * @regs: array of register state pointers to free
 */
void cla_regs_free_domain(struct cla_domain *domain, struct cla_regs **regs)
{
	int i;

	for (i = 0; i < domain->nr_devs; i++)
		kvfree(regs[i]);
	kfree(regs);
}
