// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(C) 2020 Linaro Limited. All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#include "coresight-etm4x.h"
#include "coresight-etm4x-cfg.h"
#include "coresight-priv.h"
#include "coresight-syscfg.h"

/* defines to associate register IDs with driver data locations */
#define CHECKREG(cval, elem) \
	{ \
		if (offset == cval) { \
			reg_csdev->driver_regval = &drvcfg->elem; \
			err = 0; \
			break; \
		} \
	}

#define CHECKREGIDX(cval, elem, off_idx, mask)	\
	{ \
		if (mask == cval) { \
			reg_csdev->driver_regval = &drvcfg->elem[off_idx]; \
			err = 0; \
			break; \
		} \
	}

/**
 * etm4_cfg_map_reg_offset - validate and map the register offset into a
 *			     location in the driver config struct.
 *
 * Limits the number of registers that can be accessed and programmed in
 * features, to those which are used to control the trace capture parameters.
 *
 * Omits or limits access to those which the driver must use exclusively.
 *
 * Invalid offsets will result in fail code return and feature load failure.
 *
 * @drvdata:	driver data to map into.
 * @reg_csdev:	register to map.
 * @offset:	device offset for the register
 */
static int etm4_cfg_map_reg_offset(struct etmv4_drvdata *drvdata,
				   struct cscfg_regval_csdev *reg_csdev, u32 offset)
{
	int err = -EINVAL, idx;
	struct etmv4_config *drvcfg = &drvdata->curr_config;
	u32 off_mask;

	if (((offset >= TRCEVENTCTL0R) && (offset <= TRCVIPCSSCTLR)) ||
	    ((offset >= TRCSEQRSTEVR) && (offset <= TRCEXTINSELR)) ||
	    ((offset >= TRCCIDCCTLR0) && (offset <= TRCVMIDCCTLR1))) {
		do {
			CHECKREG(TRCEVENTCTL0R, eventctrl0);
			CHECKREG(TRCEVENTCTL1R, eventctrl1);
			CHECKREG(TRCSTALLCTLR, stall_ctrl);
			CHECKREG(TRCTSCTLR, ts_ctrl);
			CHECKREG(TRCSYNCPR, syncfreq);
			CHECKREG(TRCCCCTLR, ccctlr);
			CHECKREG(TRCBBCTLR, bb_ctrl);
			CHECKREG(TRCVICTLR, vinst_ctrl);
			CHECKREG(TRCVIIECTLR, viiectlr);
			CHECKREG(TRCVISSCTLR, vissctlr);
			CHECKREG(TRCVIPCSSCTLR, vipcssctlr);
			CHECKREG(TRCSEQRSTEVR, seq_rst);
			CHECKREG(TRCSEQSTR, seq_state);
			CHECKREG(TRCEXTINSELR, ext_inp);
			CHECKREG(TRCCIDCCTLR0, ctxid_mask0);
			CHECKREG(TRCCIDCCTLR1, ctxid_mask1);
			CHECKREG(TRCVMIDCCTLR0, vmid_mask0);
			CHECKREG(TRCVMIDCCTLR1, vmid_mask1);
		} while (0);
	} else if ((offset & GENMASK(11, 4)) == TRCSEQEVRn(0)) {
		/* sequencer state control registers */
		idx = (offset & GENMASK(3, 0)) / 4;
		if (idx < ETM_MAX_SEQ_TRANSITIONS) {
			reg_csdev->driver_regval = &drvcfg->seq_ctrl[idx];
			err = 0;
		}
	} else if ((offset >= TRCSSCCRn(0)) && (offset <= TRCSSPCICRn(7))) {
		/* 32 bit, 8 off indexed register sets */
		idx = (offset & GENMASK(4, 0)) / 4;
		off_mask =  (offset & GENMASK(11, 5));
		do {
			CHECKREGIDX(TRCSSCCRn(0), ss_ctrl, idx, off_mask);
			CHECKREGIDX(TRCSSPCICRn(0), ss_pe_cmp, idx, off_mask);
		} while (0);
	} else if ((offset >= TRCCIDCVRn(0)) && (offset <= TRCVMIDCVRn(7))) {
		/* 64 bit, 8 off indexed register sets */
		idx = (offset & GENMASK(5, 0)) / 8;
		off_mask = (offset & GENMASK(11, 6));
		do {
			CHECKREGIDX(TRCCIDCVRn(0), ctxid_pid, idx, off_mask);
			CHECKREGIDX(TRCVMIDCVRn(0), vmid_val, idx, off_mask);
		} while (0);
	} else if ((offset >= TRCRSCTLRn(2)) &&
		   (offset <= TRCRSCTLRn((ETM_MAX_RES_SEL - 1)))) {
		/* 32 bit resource selection regs, 32 off, skip fixed 0,1 */
		idx = (offset & GENMASK(6, 0)) / 4;
		if (idx < ETM_MAX_RES_SEL) {
			reg_csdev->driver_regval = &drvcfg->res_ctrl[idx];
			err = 0;
		}
	} else if ((offset >= TRCACVRn(0)) &&
		   (offset <= TRCACATRn((ETM_MAX_SINGLE_ADDR_CMP - 1)))) {
		/* 64 bit addr cmp regs, 16 off */
		idx = (offset & GENMASK(6, 0)) / 8;
		off_mask = offset & GENMASK(11, 7);
		do {
			CHECKREGIDX(TRCACVRn(0), addr_val, idx, off_mask);
			CHECKREGIDX(TRCACATRn(0), addr_acc, idx, off_mask);
		} while (0);
	} else if ((offset >= TRCCNTRLDVRn(0)) &&
		   (offset <= TRCCNTVRn((ETMv4_MAX_CNTR - 1)))) {
		/* 32 bit counter regs, 4 off (ETMv4_MAX_CNTR - 1) */
		idx = (offset &  GENMASK(3, 0)) / 4;
		off_mask = offset &  GENMASK(11, 4);
		do {
			CHECKREGIDX(TRCCNTRLDVRn(0), cntrldvr, idx, off_mask);
			CHECKREGIDX(TRCCNTCTLRn(0), cntr_ctrl, idx, off_mask);
			CHECKREGIDX(TRCCNTVRn(0), cntr_val, idx, off_mask);
		} while (0);
	}
	return err;
}

static int etm4_cfg_check_eventctl0_support(const struct cscfg_regval_desc *reg_desc,
					    unsigned int nr_event)
{
	u32 requested;

	if (reg_desc->offset != TRCEVENTCTL0R)
		return 0;

	/* Parameters can change after loading, so check all writable fields. */
	if (reg_desc->type & CS_CFG_REG_TYPE_VAL_PARAM)
		requested = reg_desc->type & CS_CFG_REG_TYPE_VAL_MASK ?
			    reg_desc->mask32 : U32_MAX;
	else
		requested = reg_desc->type & CS_CFG_REG_TYPE_VAL_MASK ?
			    reg_desc->val32 & reg_desc->mask32 : reg_desc->val32;

	/* Four events use the full register: do not shift a u32 by 32. */
	if (nr_event < 4 && (requested >> (8 * nr_event)))
		return -EOPNOTSUPP;
	return 0;
}

/**
 * etm4_cfg_load_feature - load a feature into a device instance.
 *
 * @csdev:	An ETMv4 CoreSight device.
 * @feat_csdev:	The feature to be loaded.
 *
 * The function will load a feature instance into the device, checking that
 * the register definitions are valid for the device.
 *
 * Parameter and register definitions will be converted into internal
 * structures that are used to set the values in the driver when the
 * feature is enabled for the device.
 *
 * The feature spinlock pointer is initialised to the same spinlock
 * that the driver uses to protect the internal register values.
 */
static int etm4_cfg_load_feature(struct coresight_device *csdev,
				 struct cscfg_feature_csdev *feat_csdev)
{
	struct device *dev = csdev->dev.parent;
	struct etmv4_drvdata *drvdata = dev_get_drvdata(dev);
	const struct cscfg_feature_desc *feat_desc = feat_csdev->feat_desc;
	const struct cscfg_regval_desc *reg_desc;
	u32 offset;
	int i = 0, err = 0;

	/*
	 * drvdata structure of etm4 for configfs is curr_config and
	 * this curr_config is protected with csdev->mode by restricting
	 * access to curr_config after mode changed to PERF or SYSFS.
	 *
	 * Therefore, it doesn't need to protected with etmv4_drvdata->spinlock
	 * to feature applying and set the NULL.
	 */
	feat_csdev->drv_spinlock = NULL;

	/* Validate every description before deciding whether to skip a device. */
	for (i = 0; i < feat_csdev->nr_regs; i++) {
		reg_desc = &feat_desc->regs_desc[i];
		offset = reg_desc->offset;
		if ((reg_desc->type & CS_CFG_REG_TYPE_VAL_PARAM) &&
		    reg_desc->param_idx >= (u32)feat_csdev->nr_params)
			return -EINVAL;
		if (offset == TRCEVENTCTL0R &&
		    (reg_desc->type & CS_CFG_REG_TYPE_VAL_64BIT))
			return -EINVAL;
		err = etm4_cfg_map_reg_offset(drvdata, &feat_csdev->regs_csdev[i], offset);
		if (err)
			return err;
	}

	for (i = 0; i < feat_csdev->nr_regs; i++) {
		err = etm4_cfg_check_eventctl0_support(&feat_desc->regs_desc[i],
						       drvdata->caps.nr_event);
		if (err)
			return err;
	}
	return 0;
}

/* match information when loading configurations */
#define CS_CFG_ETM4_MATCH_FLAGS	(CS_CFG_MATCH_CLASS_SRC_ALL | \
				 CS_CFG_MATCH_CLASS_SRC_ETM4)

int etm4_cscfg_register(struct coresight_device *csdev)
{
	struct cscfg_csdev_feat_ops ops;

	ops.load_feat = &etm4_cfg_load_feature;

	return cscfg_register_csdev(csdev, CS_CFG_ETM4_MATCH_FLAGS, &ops);
}
