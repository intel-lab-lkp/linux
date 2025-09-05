// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI hardware device driver
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <asm/mmu.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>

#include "context.h"
#include "descriptor.h"
#include "dma.h"
#include "hw.h"
#include "error.h"
#include "sdxi.h"

enum sdxi_fn_gsv {
	SDXI_GSV_STOP,
	SDXI_GSV_INIT,
	SDXI_GSV_ACTIVE,
	SDXI_GSV_STOPG_SF,
	SDXI_GSV_STOPG_HD,
	SDXI_GSV_ERROR,
};

static const char *const gsv_strings[] = {
	[SDXI_GSV_STOP]     = "stopped",
	[SDXI_GSV_INIT]     = "initializing",
	[SDXI_GSV_ACTIVE]   = "active",
	[SDXI_GSV_STOPG_SF] = "soft stopping",
	[SDXI_GSV_STOPG_HD] = "hard stopping",
	[SDXI_GSV_ERROR]    = "error",
};

static const char *gsv_str(enum sdxi_fn_gsv gsv)
{
	if ((size_t)gsv < ARRAY_SIZE(gsv_strings))
		return gsv_strings[(size_t)gsv];

	WARN_ONCE(1, "unexpected gsv %u\n", gsv);

	return "unknown";
}

enum sdxi_fn_gsr {
	SDXI_GSRV_RESET,
	SDXI_GSRV_STOP_SF,
	SDXI_GSRV_STOP_HD,
	SDXI_GSRV_ACTIVE,
};

static enum sdxi_fn_gsv sdxi_dev_gsv(const struct sdxi_dev *sdxi)
{
	return (enum sdxi_fn_gsv)FIELD_GET(SDXI_MMIO_STS0_FN_GSV,
					sdxi_read64(sdxi, SDXI_MMIO_STS0));
}

static void sdxi_write_fn_gsr(struct sdxi_dev *sdxi, enum sdxi_fn_gsr cmd)
{
	u64 ctl0 = sdxi_read64(sdxi, SDXI_MMIO_CTL0);

	FIELD_MODIFY(SDXI_MMIO_CTL0_FN_GSR, &ctl0, cmd);
	sdxi_write64(sdxi, SDXI_MMIO_CTL0, ctl0);
}

static int sdxi_dev_start(struct sdxi_dev *sdxi)
{
	unsigned long deadline;
	enum sdxi_fn_gsv status;

	status = sdxi_dev_gsv(sdxi);
	if (status != SDXI_GSV_STOP) {
		sdxi_err(sdxi,
			 "can't activate busy device (unexpected gsv: %s)\n",
			 gsv_str(status));
		return -EIO;
	}

	sdxi_write_fn_gsr(sdxi, SDXI_GSRV_ACTIVE);

	deadline = jiffies + msecs_to_jiffies(1000);
	do {
		status = sdxi_dev_gsv(sdxi);
		sdxi_dbg(sdxi, "%s: function state: %s\n", __func__, gsv_str(status));

		switch (status) {
		case SDXI_GSV_ACTIVE:
			sdxi_dbg(sdxi, "activated\n");
			return 0;
		case SDXI_GSV_ERROR:
			sdxi_err(sdxi, "went to error state\n");
			return -EIO;
		case SDXI_GSV_INIT:
		case SDXI_GSV_STOP:
			/* transitional states, wait */
			fsleep(1000);
			break;
		default:
			sdxi_err(sdxi, "unexpected gsv %u, giving up\n", status);
			return -EIO;
		}
	} while (time_before(jiffies, deadline));

	sdxi_err(sdxi, "activation timed out, current status %u\n",
		sdxi_dev_gsv(sdxi));
	return -ETIMEDOUT;

}

/* Get the device to the GSV_STOP state. */
static int sdxi_dev_stop(struct sdxi_dev *sdxi)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	bool reset_issued = false;

	do {
		enum sdxi_fn_gsv status = sdxi_dev_gsv(sdxi);

		sdxi_dbg(sdxi, "%s: function state: %s\n", __func__, gsv_str(status));

		switch (status) {
		case SDXI_GSV_ACTIVE:
			sdxi_write_fn_gsr(sdxi, SDXI_GSRV_STOP_SF);
			break;
		case SDXI_GSV_ERROR:
			if (!reset_issued) {
				sdxi_info(sdxi,
					  "function in error state, issuing reset\n");
				sdxi_write_fn_gsr(sdxi, SDXI_GSRV_RESET);
				reset_issued = true;
			} else {
				fsleep(1000);
			}
			break;
		case SDXI_GSV_STOP:
			return 0;
		case SDXI_GSV_INIT:
		case SDXI_GSV_STOPG_SF:
		case SDXI_GSV_STOPG_HD:
			/* transitional states, wait */
			sdxi_dbg(sdxi, "waiting for stop (gsv = %u)\n",
				 status);
			fsleep(1000);
			break;
		default:
			sdxi_err(sdxi, "unknown gsv %u, giving up\n", status);
			return -EIO;
		}
	} while (time_before(jiffies, deadline));

	sdxi_err(sdxi, "stop attempt timed out, current status %u\n",
		sdxi_dev_gsv(sdxi));
	return -ETIMEDOUT;
}

static void sdxi_stop(struct sdxi_dev *sdxi)
{
	sdxi_dev_stop(sdxi);
}

/* Refer to "Activation of the SDXI Function by Software". */
static int sdxi_fn_activate(struct sdxi_dev *sdxi)
{
	const struct sdxi_dev_ops *ops = sdxi->dev_ops;
	u64 cxt_l2;
	u64 cap0;
	u64 cap1;
	u64 ctl2;
	int err;

	/*
	 * Clear any existing configuration from MMIO_CTL0 and ensure
	 * the function is in GSV_STOP state.
	 */
	sdxi_write64(sdxi, SDXI_MMIO_CTL0, 0);
	err = sdxi_dev_stop(sdxi);
	if (err)
		return err;

	/*
	 * 1.a. Discover limits and implemented features via MMIO_CAP0
	 * and MMIO_CAP1.
	 */
	cap0 = sdxi_read64(sdxi, SDXI_MMIO_CAP0);
	sdxi->sfunc = FIELD_GET(SDXI_MMIO_CAP0_SFUNC, cap0);
	sdxi->max_ring_entries = SZ_1K;
	sdxi->max_ring_entries *= 1U << FIELD_GET(SDXI_MMIO_CAP0_MAX_DS_RING_SZ, cap0);
	sdxi->db_stride = SZ_4K;
	sdxi->db_stride *= 1U << FIELD_GET(SDXI_MMIO_CAP0_DB_STRIDE, cap0);

	cap1 = sdxi_read64(sdxi, SDXI_MMIO_CAP1);
	sdxi->max_akeys = SZ_256;
	sdxi->max_akeys *= 1U << FIELD_GET(SDXI_MMIO_CAP1_MAX_AKEY_SZ, cap1);
	sdxi->max_cxts = 1 + FIELD_GET(SDXI_MMIO_CAP1_MAX_CXT, cap1);
	sdxi->op_grp_cap = FIELD_GET(SDXI_MMIO_CAP1_OPB_000_CAP, cap1);

	/*
	 * 1.b. Configure SDXI parameters via MMIO_CTL2. We don't have
	 * any reason to impose more conservative limits than those
	 * reported in the capability registers, so use those for now.
	 */
	ctl2 = 0;
	ctl2 |= FIELD_PREP(SDXI_MMIO_CTL2_MAX_BUFFER,
			   FIELD_GET(SDXI_MMIO_CAP1_MAX_BUFFER, cap1));
	ctl2 |= FIELD_PREP(SDXI_MMIO_CTL2_MAX_AKEY_SZ,
			   FIELD_GET(SDXI_MMIO_CAP1_MAX_AKEY_SZ, cap1));
	ctl2 |= FIELD_PREP(SDXI_MMIO_CTL2_MAX_CXT,
			   FIELD_GET(SDXI_MMIO_CAP1_MAX_CXT, cap1));
	ctl2 |= FIELD_PREP(SDXI_MMIO_CTL2_OPB_000_AVL,
			   FIELD_GET(SDXI_MMIO_CAP1_OPB_000_CAP, cap1));
	sdxi_write64(sdxi, SDXI_MMIO_CTL2, ctl2);

	sdxi_dbg(sdxi,
		 "sfunc:%#x descmax:%llu dbstride:%#x akeymax:%u cxtmax:%u opgrps:%#x\n",
		 sdxi->sfunc, sdxi->max_ring_entries, sdxi->db_stride,
		 sdxi->max_akeys, sdxi->max_cxts, sdxi->op_grp_cap);

	/* 2.a-2.b. Allocate and zero the 4KB Context Level 2 Table */
	sdxi->l2_table = dmam_alloc_coherent(sdxi_to_dev(sdxi), L2_TABLE_SIZE,
					     &sdxi->l2_dma, GFP_KERNEL);
	if (!sdxi->l2_table)
		return -ENOMEM;

	/* 2.c. Program MMIO_CXT_L2 */
	cxt_l2 = FIELD_PREP(SDXI_MMIO_CXT_L2_PTR, sdxi->l2_dma >> ilog2(SZ_4K));
	sdxi_write64(sdxi, SDXI_MMIO_CXT_L2, cxt_l2);

	/*
	 * 2.c.i. TODO: Program MMIO_CTL0.fn_pasid and
	 * MMIO_CTL0.fn_pasid if guest virtual addressing required.
	 */

	/*
	 * This covers the following steps:
	 *
	 * 3. Context Level 1 Table Setup for contexts 0..127.
	 * 4.a. Create the administrative context and associated control
	 * structures.
	 * 4.b. Set its CXT_STS.state to CXTV_RUN; see 10.b.
	 *
	 * The admin context will not consume descriptors until we
	 * write its doorbell later.
	 */
	sdxi->admin_cxt = sdxi_working_cxt_init(sdxi, SDXI_ADMIN_CXT_ID);
	if (!sdxi->admin_cxt)
		return -ENOMEM;
	/*
	 * 5. Mailbox: we don't use this facility and we assume the
	 * reset values are sane.
	 *
	 * 6. If restoring saved state, adjust as appropriate. (We're not.)
	 */

	/*
	 * MSI allocation is informed by the function's maximum
	 * supported contexts, which was discovered in 1.a. Need to do
	 * this before step 7, which claims an IRQ.
	 */
	err = (ops && ops->irq_init) ? ops->irq_init(sdxi) : 0;
	if (err)
		goto admin_cxt_exit;

	/* 7. Initialize error log according to "Error Log Initialization". */
	err = sdxi_error_init(sdxi);
	if (err)
		goto irq_exit;

	/*
	 * 8. "Software may also need to configure and enable
	 * additional [features]". We've already performed MSI setup,
	 * nothing else for us to do here for now.
	 */

	/*
	 * 9. Set MMIO_CTL0.fn_gsr to GSRV_ACTIVE and wait for
	 * MMIO_STS0.fn_gsv to reach GSV_ACTIVE or GSV_ERROR.
	 */
	err = sdxi_dev_start(sdxi);
	if (err)
		goto error_exit;

	/*
	 * 10. Jump start the admin context. This step refers to
	 * "Starting A context and Context Signaling," where method #3
	 * recommends writing an "appropriate" value to the doorbell
	 * register. We haven't queued any descriptors to the admin
	 * context at this point, so the appropriate value would be 0.
	 */
	iowrite64(0, sdxi->admin_cxt->db);

	return 0;

error_exit:
	sdxi_error_exit(sdxi);
irq_exit:
	if (ops && ops->irq_exit)
		ops->irq_exit(sdxi);
admin_cxt_exit:
	sdxi_working_cxt_exit(sdxi->admin_cxt);
	return err;
}

int sdxi_device_init(struct sdxi_dev *sdxi, const struct sdxi_dev_ops *ops)
{
	struct sdxi_cxt_start params;
	struct sdxi_cxt *admin_cxt;
	struct sdxi_desc desc;
	struct sdxi_cxt *dma_cxt;
	int err;

	sdxi->dev_ops = ops;

	sdxi->write_index_pool = dmam_pool_create("Write_Index", sdxi_to_dev(sdxi),
						  sizeof(__le64), sizeof(__le64), 0);
	sdxi->cxt_sts_pool = dmam_pool_create("CXT_STS", sdxi_to_dev(sdxi),
					      sizeof(struct sdxi_cxt_sts),
					      sizeof(struct sdxi_cxt_sts), 0);
	sdxi->cxt_ctl_pool = dmam_pool_create("CXT_CTL", sdxi_to_dev(sdxi),
					      sizeof(struct sdxi_cxt_ctl),
					      sizeof(struct sdxi_cxt_ctl), 0);
	if (!sdxi->write_index_pool || !sdxi->cxt_sts_pool || !sdxi->cxt_ctl_pool)
		return -ENOMEM;

	err = sdxi_fn_activate(sdxi);
	if (err)
		return err;

	admin_cxt = sdxi->admin_cxt;

	dma_cxt = sdxi_working_cxt_init(sdxi, SDXI_DMA_CXT_ID);
	if (!dma_cxt) {
		err = -EINVAL;
		goto fn_stop;
	}

	sdxi->dma_cxt = dma_cxt;

	params = (typeof(params)) {
		.range = sdxi_cxt_range(dma_cxt->id),
	};
	err = sdxi_encode_cxt_start(&desc, &params);
	if (err)
		goto fn_stop;

	err = sdxi_submit_desc(admin_cxt, &desc);
	if (err)
		goto fn_stop;

	sdxi_dma_register(sdxi->dma_cxt);

	return 0;
fn_stop:
	sdxi_stop(sdxi);
	return err;
}

void sdxi_device_exit(struct sdxi_dev *sdxi)
{
	sdxi_dma_unregister(sdxi->dma_cxt);
	sdxi_working_cxt_exit(sdxi->dma_cxt);

	/* Walk sdxi->cxt_array freeing any allocated rows. */
	for (size_t i = 0; i < L2_TABLE_ENTRIES; ++i) {
		if (!sdxi->cxt_array[i])
			continue;
		/* When a context is released its entry in the table should be NULL. */
		for (size_t j = 0; j < L1_TABLE_ENTRIES; ++j) {
			struct sdxi_cxt *cxt = sdxi->cxt_array[i][j];

			if (!cxt)
				continue;
			if (cxt->id != 0)  /* admin context shutdown is last */
				sdxi_working_cxt_exit(cxt);
			sdxi->cxt_array[i][j] = NULL;
		}
		if (i != 0)  /* another special case for admin cxt */
			kfree(sdxi->cxt_array[i]);
	}

	sdxi_working_cxt_exit(sdxi->admin_cxt);
	kfree(sdxi->cxt_array[0]);  /* ugh */

	sdxi_stop(sdxi);
	sdxi_error_exit(sdxi);
	if (sdxi->dev_ops && sdxi->dev_ops->irq_exit)
		sdxi->dev_ops->irq_exit(sdxi);
}

MODULE_DESCRIPTION("SDXI driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Wei Huang");
MODULE_AUTHOR("Nathan Lynch");
