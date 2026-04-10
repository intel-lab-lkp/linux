// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI hardware device driver
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/log2.h>
#include <linux/slab.h>

#include "context.h"
#include "hw.h"
#include "mmio.h"
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

/*
 * See SDXI 1.0 4.1.8 Activation of the SDXI Function by Software.
 */
static int sdxi_fn_activate(struct sdxi_dev *sdxi)
{
	u64 version, cap0, cap1, ctl2, cxt_l2, lv01_ptr;
	struct sdxi_cxt_L2_ent *L2_ent;
	int err;

	/*
	 * Clear any existing configuration from MMIO_CTL0 and ensure
	 * the function is in GSV_STOP state.
	 */
	sdxi_write64(sdxi, SDXI_MMIO_CTL0, 0);
	err = sdxi_dev_stop(sdxi);
	if (err)
		return err;

	version = sdxi_read64(sdxi, SDXI_MMIO_VERSION);
	sdxi_info(sdxi, "SDXI %llu.%llu device found\n",
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
	sdxi->L2_table = dmam_alloc_coherent(sdxi_to_dev(sdxi),
					     sizeof(*sdxi->L2_table),
					     &sdxi->L2_dma, GFP_KERNEL);
	if (!sdxi->L2_table)
		return -ENOMEM;

	cxt_l2 = FIELD_PREP(SDXI_MMIO_CXT_L2_PTR, sdxi->L2_dma >> ilog2(SZ_4K));
	sdxi_write64(sdxi, SDXI_MMIO_CXT_L2, cxt_l2);

	/* SDXI 1.0 4.1.8.3 Context Level 1 Table Setup */
	sdxi->L1_table = dmam_alloc_coherent(sdxi_to_dev(sdxi),
					     sizeof(*sdxi->L1_table),
					     &sdxi->L1_dma, GFP_KERNEL);
	if (!sdxi->L1_table)
		return -ENOMEM;
	/*
	 * SDXI 1.0 4.1.8.3.c: Initialize the Context level 2 table to
	 * point to the Context Level 1 [table].
	 */
	L2_ent = &sdxi->L2_table->entry[0];
	lv01_ptr = FIELD_PREP(SDXI_CXT_L2_ENT_VL, 1);
	lv01_ptr |= FIELD_PREP(SDXI_CXT_L2_ENT_LV01_PTR,
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

	/*
	 * SDXI 1.0 4.1.8.9: Set MMIO_CTL0.fn_gsr to GSRV_ACTIVE and
	 * wait for MMIO_STS0.fn_gsv to reach GSV_ACTIVE or GSV_ERROR.
	 */
	return sdxi_dev_start(sdxi);
}

static int sdxi_create_dma_pool(struct sdxi_dev *sdxi, struct dma_pool **pool,
				const char *name, size_t size)
{
	*pool = dmam_pool_create(name, sdxi_to_dev(sdxi), size, size, 0);
	return *pool ? 0 : -ENOMEM;
}

static int sdxi_device_init(struct sdxi_dev *sdxi)
{
	int err;

	if (sdxi_create_dma_pool(sdxi, &sdxi->write_index_pool,
				 "Write_Index", sizeof(__le64)))
		return -ENOMEM;
	if (sdxi_create_dma_pool(sdxi, &sdxi->cxt_sts_pool,
				 "CXT_STS", sizeof(struct sdxi_cxt_sts)))
		return -ENOMEM;
	if (sdxi_create_dma_pool(sdxi, &sdxi->cxt_ctl_pool,
				 "CXT_CTL", sizeof(struct sdxi_cxt_ctl)))
		return -ENOMEM;
	if (sdxi_create_dma_pool(sdxi, &sdxi->cst_blk_pool,
				 "CST_BLK", sizeof(struct sdxi_cst_blk)))
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

void sdxi_unregister(struct device *dev)
{
	struct sdxi_dev *sdxi = dev_get_drvdata(dev);

	sdxi_dev_stop(sdxi);
}
