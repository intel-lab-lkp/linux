// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arm CLA driver - Launch operations
 *
 * Copyright 2026 Arm Limited.
 */

#include <linux/iopoll.h>

#include "arm-cla.h"

/* Time to wait between two LRESP reads */
#define CLA_LRESP_DELAY_US	1

/*
 * Time to wait for LRESP_PENDING to clear. Commands like REGREAD should
 * complete in a few cycles, but ENTERSR and RESET may need to clean up
 * some very large states depending on the work interrupted, and may need
 * 1us or more.
 */
#define CLA_LRESP_TIMEOUT_US	100

enum cla_launch_data_mode {
	CLA_DATA_NONE,
	CLA_DATA_IN,
	CLA_DATA_OUT,
};

struct cla_launch {
	u8	op;		/* opcode */
	u8	ndata_m1;	/* Data size minus 1 */
	u8	accid;		/* Accelerator ID */
	bool	seq;		/* part of compound cmd */
	u32	regidx;		/* Register index */

	enum cla_launch_data_mode data_mode;
	u64	*data;		/* In/out data */

	u8	errcode;	/* Output error code */
};

/**
 * cla_op_launch - Launch operation.
 * @dev: CLA device.
 * @launch: LAUNCH settings.
 *
 * 1. If data_mode is %CLA_DATA_IN, write DATA registers.
 * 2. Launch the operation, and wait for the response.
 * 3. If data_mode is %CLA_DATA_OUT, read DATA registers into @launch->data.
 *
 * Return:
 * * %0			- Success.
 * * %-ETIMEDOUT	- LAUNCH timed out (possibly no CLA at this address).
 *			  Unless LRESP_PENDING eventually clears, this is
 *			  unrecoverable.
 * * %-ENODEV		- Accelerator not available.
 * * %-EBUSY		- Accelerator is busy.
 * * %-EIO		- LAUNCH error. @launch->errcode contains the LRESP
 *			  error code.
 */
static int cla_op_launch(struct cla_dev *dev, struct cla_launch *launch)
{
	int i;
	int ret;
	u64 lresp;

	if (WARN_ON(smp_processor_id() != dev->cpu))
		return -EINVAL;

	if (launch->data_mode == CLA_DATA_IN)
		for (i = 0; i < launch->ndata_m1 + 1; i++)
			cla_reg_write(dev, CLA_REG_DATA(i), launch->data[i]);

	/*
	 * No barrier needed because accesses use Device-nGnRE, within the same
	 * memory-mapped peripheral, so accesses arrive at the endpoint in
	 * program order.
	 */
	cla_reg_write(dev, CLA_REG_LAUNCH,
		      FIELD_PREP(CLA_LAUNCH_OP, launch->op) |
		      FIELD_PREP(CLA_LAUNCH_NDATA_M1, launch->ndata_m1) |
		      FIELD_PREP(CLA_LAUNCH_ACCID, launch->accid) |
		      FIELD_PREP(CLA_LAUNCH_SEQ, launch->seq) |
		      FIELD_PREP(CLA_LAUNCH_REGIDX, launch->regidx));

	ret = cla_op_wait_lresp(dev, &lresp);
	if (ret) {
		cla_err(dev, "launch failed with %d\n", ret);
		return ret;
	}

	switch (FIELD_GET(CLA_LRESP_CODE, lresp)) {
	case CLA_LRESP_OK:
		break;
	case CLA_LRESP_UNAVAIL:
		return -ENODEV;
	case CLA_LRESP_BUSY:
		return -EBUSY;
	case CLA_LRESP_ERROR:
		launch->errcode = FIELD_GET(CLA_LRESP_ERRCODE, lresp);
		return -EIO;
	}

	if (launch->data_mode == CLA_DATA_OUT)
		for (i = 0; i < launch->ndata_m1 + 1; i++)
			launch->data[i] = cla_reg_read(dev, CLA_REG_DATA(i));

	return 0;
}

/**
 * cla_op_wait_lresp - Wait for any LAUNCH op to complete.
 * @dev: CLA device.
 * @lresp: last LRESP value read.
 *
 * Return: 0 on success, -ETIMEDOUT in case of timeout.
 */
int cla_op_wait_lresp(struct cla_dev *dev, u64 *lresp)
{
	return readq_relaxed_poll_timeout_atomic(dev->regs + CLA_REG_LRESP,
			*lresp, FIELD_GET(CLA_LRESP_PENDING, *lresp) == 0,
			CLA_LRESP_DELAY_US, CLA_LRESP_TIMEOUT_US);
}

/**
 * cla_op_reset - Launch RESET operation for this accelerator.
 * @dev: CLA device.
 * @accid: accelerator ID.
 *
 * Return: 0 on success, 1 if there is no accelerator with this ID, or an error.
 */
int cla_op_reset(struct cla_dev *dev, unsigned int accid)
{
	int ret;
	struct cla_launch launch = {
		.op	= CLA_LAUNCH_OP_RESET,
		.accid	= accid,
	};

	ret = cla_op_launch(dev, &launch);
	if (ret == -EIO && launch.errcode == CLA_ERRCODE_NOACC)
		return 1;
	return ret;
}

/**
 * cla_op_reset_all - Reset all attached accelerators
 * @dev: CLA device.
 *
 * Return: 0 on success, or an error
 */
int cla_op_reset_all(struct cla_dev *dev)
{
	int ret;
	unsigned int accid;

	cla_for_each_accid(dev, accid) {
		ret = cla_op_reset(dev, accid);
		if (ret)
			return ret < 0 ? ret : -ENODEV;
	}
	return 0;
}

static int cla_op_access_reg(struct cla_dev *dev, u8 op,
			     enum cla_launch_data_mode data_mode,
			     unsigned int accid, unsigned int regidx,
			     size_t nregs, u64 *regs)
{
	int ret = -EINVAL;
	unsigned long max_regidx;
	struct cla_launch launch = {
		.op = op,
		.accid = accid,
		.data_mode = data_mode,
	};

	switch (op) {
	case CLA_LAUNCH_OP_REGREAD:
	case CLA_LAUNCH_OP_REGWRITE:
		max_regidx = 0x100000000;
		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	if (WARN_ON(regidx + nregs > max_regidx))
		return -EINVAL;

	/* 1 to 8 registers accessed at a time, within the same 8-reg group */
	while (nregs > 0) {
		unsigned int reg_group = ALIGN_DOWN(regidx, 8);
		unsigned int max_reg = min(regidx + nregs, reg_group + 8);
		unsigned int ndata = max_reg - regidx;

		launch.ndata_m1 = ndata - 1;
		launch.regidx = regidx;
		launch.data = regs;

		ret = cla_op_launch(dev, &launch);
		if (ret)
			break;

		regidx += ndata;
		regs += ndata;
		nregs -= ndata;
	}

	return ret;
}

/**
 * cla_op_regread - Launch REGREAD operations.
 * @dev: CLA device.
 * @accid: accelerator ID.
 * @regidx: first register index.
 * @nregs: number of registers. Can be greater than 8 (accessed with multiple
 *         REGREAD operations).
 * @regs: array of length @nregs.
 *
 * Return: 0 on success, or an error.
 */
int cla_op_regread(struct cla_dev *dev, unsigned int accid,
			  unsigned int regidx, size_t nregs, u64 *regs)
{
	return cla_op_access_reg(dev, CLA_LAUNCH_OP_REGREAD, CLA_DATA_OUT,
				 accid, regidx, nregs, regs);
}

/**
 * cla_op_regwrite - Launch REGWRITE operations.
 * @dev: CLA device.
 * @accid: accelerator ID.
 * @regidx: first register index.
 * @nregs: number of registers. Can be greater than 8 (accessed with multiple
 *         REGWRITE operations).
 * @regs: array of length @nregs.
 *
 * Return: 0 on success, or an error.
 */
int cla_op_regwrite(struct cla_dev *dev, unsigned int accid,
			   unsigned int regidx, size_t nregs, u64 *regs)
{
	return cla_op_access_reg(dev, CLA_LAUNCH_OP_REGWRITE, CLA_DATA_IN,
				 accid, regidx, nregs, regs);
}
