// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI hardware device driver
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#include <linux/bitfield.h>
#include <linux/cache.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/export.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/time.h>

#include "context.h"
#include "hw.h"
#include "mmio.h"
#include "sdxi.h"

enum sdxi_fn_gsv {
	SDXI_GSV_STOP     = 0,
	SDXI_GSV_INIT     = 1,
	SDXI_GSV_ACTIVE   = 2,
	SDXI_GSV_STOPG_SF = 3,
	SDXI_GSV_STOPG_HD = 4,
	SDXI_GSV_ERROR    = 5,
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
	SDXI_GSRV_RESET   = 0,
	SDXI_GSRV_STOP_SF = 1,
	SDXI_GSRV_STOP_HD = 2,
	SDXI_GSRV_ACTIVE  = 3,
};

static enum sdxi_fn_gsv sdxi_dev_gsv(const struct sdxi_dev *sdxi)
{
	u64 sts0 = sdxi_read64(sdxi, SDXI_MMIO_STS0);
	enum sdxi_fn_gsv gsv = FIELD_GET(SDXI_MMIO_STS0_FN_GSV, sts0);

	switch (gsv) {
	case SDXI_GSV_STOP ... SDXI_GSV_ERROR:
		break;
	default:
		dev_warn_ratelimited(sdxi->dev, "unknown gsv %u\n", gsv);
		break;
	}

	return gsv;
}

static const unsigned long gsv_poll_interval_us = USEC_PER_MSEC;
static const unsigned long gsv_transition_timeout_us = USEC_PER_SEC;

#define sdxi_dev_gsv_poll(sdxi, val, cond)				\
	read_poll_timeout(sdxi_dev_gsv, val, cond, gsv_poll_interval_us, \
			  gsv_transition_timeout_us, false, sdxi)

static void sdxi_write_fn_gsr(struct sdxi_dev *sdxi, enum sdxi_fn_gsr cmd)
{
	u64 ctl0 = sdxi_read64(sdxi, SDXI_MMIO_CTL0);

	FIELD_MODIFY(SDXI_MMIO_CTL0_FN_GSR, &ctl0, cmd);
	sdxi_write64(sdxi, SDXI_MMIO_CTL0, ctl0);
}

/* Get the device to the GSV_STOP state. */
static int sdxi_dev_stop(struct sdxi_dev *sdxi)
{
	enum sdxi_fn_gsv status = sdxi_dev_gsv(sdxi);
	int ret;

	dev_dbg(sdxi->dev, "attempting stop, current state: %s\n",
		gsv_str(status));

	switch (status) {
	case SDXI_GSV_INIT:
	case SDXI_GSV_ACTIVE:
		sdxi_write_fn_gsr(sdxi, SDXI_GSRV_STOP_SF);
		break;
	case SDXI_GSV_STOPG_SF:
		sdxi_write_fn_gsr(sdxi, SDXI_GSRV_STOP_HD);
		break;
	case SDXI_GSV_STOPG_HD:
	case SDXI_GSV_ERROR:
		/*
		 * If hard-stopping, there's nothing to do but wait.
		 * If in error state, the reset is issued below.
		 */
		break;
	default:
		/* Unrecognized state; try a reset. */
		sdxi_write_fn_gsr(sdxi, SDXI_GSRV_RESET);
		break;
	}

	/* Wait for transition to either stop or error state. */
	ret = sdxi_dev_gsv_poll(sdxi, status,
				status == SDXI_GSV_STOP ||
				status == SDXI_GSV_ERROR);

	if (ret == 0 && status == SDXI_GSV_ERROR) {
		sdxi_write_fn_gsr(sdxi, SDXI_GSRV_RESET);
		ret = sdxi_dev_gsv_poll(sdxi, status, status == SDXI_GSV_STOP);
	}

	if (ret) {
		dev_err(sdxi->dev, "stop timed out, current state: %s\n",
			gsv_str(status));
		return ret;
	}

	return 0;
}

/*
 * See SDXI 1.0 4.1.8 Activation of the SDXI Function by Software.
 */
static int sdxi_fn_activate(struct sdxi_dev *sdxi)
{
	u64 version, cap0, cap1, ctl0, ctl2, cxt_l2, lv01_ptr;
	struct sdxi_cxt_L2_ent *L2_ent;
	int err;

	/*
	 * Ensure the function is in GSV_STOP state, then clear ctl0's
	 * pasid and error interrupt configuration while preserving
	 * any assigned group ID (fn_grp_id).
	 */
	err = sdxi_dev_stop(sdxi);
	if (err)
		return err;

	ctl0 = sdxi_read64(sdxi, SDXI_MMIO_CTL0);
	FIELD_MODIFY(SDXI_MMIO_CTL0_FN_ERR_INTR_EN, &ctl0, 0);
	FIELD_MODIFY(SDXI_MMIO_CTL0_FN_PASID_VL, &ctl0, 0);
	FIELD_MODIFY(SDXI_MMIO_CTL0_FN_PASID, &ctl0, 0);
	sdxi_write64(sdxi, SDXI_MMIO_CTL0, ctl0);

	version = sdxi_read64(sdxi, SDXI_MMIO_VERSION);
	dev_info(sdxi->dev, "SDXI %llu.%llu device found\n",
		  FIELD_GET(SDXI_MMIO_VERSION_MAJOR, version),
		  FIELD_GET(SDXI_MMIO_VERSION_MINOR, version));

	/* Read capabilities and features. */
	cap0 = sdxi_read64(sdxi, SDXI_MMIO_CAP0);
	sdxi->db_stride = SZ_4K;
	sdxi->db_stride *= 1U << FIELD_GET(SDXI_MMIO_CAP0_DB_STRIDE, cap0);

	cap1 = sdxi_read64(sdxi, SDXI_MMIO_CAP1);
	sdxi->op_grp_cap = FIELD_GET(SDXI_MMIO_CAP1_OPB_000_CAP, cap1);

	/*
	 * Constrain the number of client contexts supported by the
	 * driver to what fits in a single L1 table.
	 */
	sdxi->max_cxtid = min(SDXI_L1_TABLE_ENTRIES - 1,
			      FIELD_GET(SDXI_MMIO_CAP1_MAX_CXT, cap1));

	/* Apply our configuration. */
	ctl2 = FIELD_PREP(SDXI_MMIO_CTL2_MAX_CXT, sdxi->max_cxtid);
	ctl2 |= FIELD_PREP(SDXI_MMIO_CTL2_MAX_BUFFER,
			   FIELD_GET(SDXI_MMIO_CAP1_MAX_BUFFER, cap1));
	ctl2 |= FIELD_PREP(SDXI_MMIO_CTL2_MAX_AKEY_SZ,
			   FIELD_GET(SDXI_MMIO_CAP1_MAX_AKEY_SZ, cap1));
	ctl2 |= FIELD_PREP(SDXI_MMIO_CTL2_OPB_000_AVL,
			   FIELD_GET(SDXI_MMIO_CAP1_OPB_000_CAP, cap1));
	sdxi_write64(sdxi, SDXI_MMIO_CTL2, ctl2);

	/* SDXI 1.0 4.1.8.2 Context Level 2 Table Setup */
	sdxi->L2_table = dmam_alloc_coherent(sdxi->dev,
					     sizeof(*sdxi->L2_table),
					     &sdxi->L2_dma, GFP_KERNEL);
	if (!sdxi->L2_table)
		return -ENOMEM;

	cxt_l2 = FIELD_PREP(SDXI_MMIO_CXT_L2_PTR, sdxi->L2_dma >> ilog2(SZ_4K));
	sdxi_write64(sdxi, SDXI_MMIO_CXT_L2, cxt_l2);

	/* SDXI 1.0 4.1.8.3 Context Level 1 Table Setup */
	sdxi->L1_table = dmam_alloc_coherent(sdxi->dev,
					     sizeof(*sdxi->L1_table),
					     &sdxi->L1_dma, GFP_KERNEL);
	if (!sdxi->L1_table)
		return -ENOMEM;
	/*
	 * SDXI 1.0 4.1.8.3.c: Initialize the Context level 2 table to
	 * point to the Context Level 1 [table].
	 */
	L2_ent = &sdxi->L2_table->entry[0];
	lv01_ptr = FIELD_PREP(SDXI_CXT_L2_ENT_VL, 1) |
		   FIELD_PREP(SDXI_CXT_L2_ENT_LV01_PTR,
			      sdxi->L1_dma >> ilog2(SZ_4K));
	L2_ent->lv01_ptr = cpu_to_le64(lv01_ptr);

	/*
	 * SDXI 1.0 4.1.8.4 Administrative Context
	 *
	 * The admin context will not consume descriptors until we
	 * write its doorbell later.
	 */
	err = sdxi_admin_cxt_init(sdxi);
	if (err)
		return err;

	return 0;
}

static int sdxi_device_init(struct sdxi_dev *sdxi)
{
	struct device *dev = sdxi->dev;
	size_t size, align;
	int err;

	size = sizeof(__le64);
	align = max(size, SMP_CACHE_BYTES);
	sdxi->write_index_pool = dmam_pool_create("Write_Index", dev, size,
						  align, 0);
	if (!sdxi->write_index_pool)
		return -ENOMEM;

	size = sizeof(struct sdxi_cxt_sts);
	align = max(size, SMP_CACHE_BYTES);
	sdxi->cxt_sts_pool = dmam_pool_create("CXT_STS", dev, size, align, 0);
	if (!sdxi->cxt_sts_pool)
		return -ENOMEM;

	size = align = sizeof(struct sdxi_cxt_ctl);
	sdxi->cxt_ctl_pool = dmam_pool_create("CXT_CTL", dev, size, align, 0);
	if (!sdxi->cxt_ctl_pool)
		return -ENOMEM;

	size = sizeof(struct sdxi_cst_blk);
	align = max(size, SMP_CACHE_BYTES);
	sdxi->cst_blk_pool = dmam_pool_create("CST_BLK", dev, size, align, 0);
	if (!sdxi->cst_blk_pool)
		return -ENOMEM;

	err = sdxi_fn_activate(sdxi);
	if (err)
		return err;

	return 0;
}

int sdxi_register(struct device *dev, const struct sdxi_bus_ops *ops)
{
	struct sdxi_dev *sdxi;
	int err;

	sdxi = devm_kzalloc(dev, sizeof(*sdxi), GFP_KERNEL);
	if (!sdxi)
		return -ENOMEM;

	sdxi->dev = dev;
	sdxi->bus_ops = ops;
	dev_set_drvdata(dev, sdxi);

	err = sdxi->bus_ops->init(sdxi);
	if (err)
		return err;

	return sdxi_device_init(sdxi);
}
EXPORT_SYMBOL_NS_GPL(sdxi_register, "SDXI");

MODULE_AUTHOR("Wei Huang");
MODULE_AUTHOR("Nathan Lynch");
MODULE_DESCRIPTION("SDXI core");
MODULE_LICENSE("GPL");
