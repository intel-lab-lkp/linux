// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014-2015, 2020 The Linux Foundation. All rights reserved.
 * Copyright (c) 2015, 2021 Linaro Limited.
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */

#include <linux/pci.h>
#include <linux/interconnect.h>
#include <linux/pm_opp.h>
#include <linux/units.h>

#include "../../pci.h"
#include "pcie-designware.h"
#include "pcie-qcom-common.h"

void qcom_pcie_common_set_16gt_eq_settings(struct dw_pcie *pci)
{
	u32 reg;

	/*
	 * GEN3_RELATED_OFF register is repurposed to apply equalization
	 * settings at various data transmission rates through registers
	 * namely GEN3_EQ_*. RATE_SHADOW_SEL bit field of GEN3_RELATED_OFF
	 * determines data rate for which this equalization settings are
	 * applied.
	 */
	reg = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
	reg &= ~GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL;
	reg &= ~GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK;
	reg |= FIELD_PREP(GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK, 0x1);
	dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, reg);

	reg = dw_pcie_readl_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF);
	reg &= ~(GEN3_EQ_FMDC_T_MIN_PHASE23 |
		GEN3_EQ_FMDC_N_EVALS |
		GEN3_EQ_FMDC_MAX_PRE_CUSROR_DELTA |
		GEN3_EQ_FMDC_MAX_POST_CUSROR_DELTA);
	reg |= FIELD_PREP(GEN3_EQ_FMDC_T_MIN_PHASE23, 0x1) |
		FIELD_PREP(GEN3_EQ_FMDC_N_EVALS, 0xd) |
		FIELD_PREP(GEN3_EQ_FMDC_MAX_PRE_CUSROR_DELTA, 0x5) |
		FIELD_PREP(GEN3_EQ_FMDC_MAX_POST_CUSROR_DELTA, 0x5);
	dw_pcie_writel_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF, reg);

	reg = dw_pcie_readl_dbi(pci, GEN3_EQ_CONTROL_OFF);
	reg &= ~(GEN3_EQ_CONTROL_OFF_FB_MODE |
		GEN3_EQ_CONTROL_OFF_PHASE23_EXIT_MODE |
		GEN3_EQ_CONTROL_OFF_FOM_INC_INITIAL_EVAL |
		GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC);
	dw_pcie_writel_dbi(pci, GEN3_EQ_CONTROL_OFF, reg);
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_set_16gt_eq_settings);

struct icc_path *qcom_pcie_common_icc_get_resource(struct dw_pcie *pci, const char *path)
{
	struct icc_path *icc_p;

	icc_p = devm_of_icc_get(pci->dev, path);
	return icc_p;
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_icc_get_resource);

int qcom_pcie_common_icc_init(struct dw_pcie *pci, struct icc_path *icc, u32 bandwidth)
{
	int ret;

	ret = icc_set_bw(icc, 0, bandwidth);
	if (ret) {
		dev_err(pci->dev, "Failed to set interconnect bandwidth: %d\n",
			ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_icc_init);

void qcom_pcie_common_icc_update(struct dw_pcie *pci, struct icc_path *icc_mem)
{
	u32 offset, status, width, speed;
	unsigned long freq_kbps;
	struct dev_pm_opp *opp;
	int ret, freq_mbps;

	if (!icc_mem)
		return;

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	status = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA);

	/* Only update constraints if link is up. */
	if (!(status & PCI_EXP_LNKSTA_DLLLA))
		return;

	speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, status);
	width = FIELD_GET(PCI_EXP_LNKSTA_NLW, status);

	if (icc_mem) {
		ret = icc_set_bw(icc_mem, 0,
				 width * QCOM_PCIE_LINK_SPEED_TO_BW(speed));
		if (ret) {
			dev_err(pci->dev, "Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
				ret);
		}
	} else {
		freq_mbps = pcie_dev_speed_mbps(pcie_link_speed[speed]);
		if (freq_mbps < 0)
			return;

		freq_kbps = freq_mbps * KILO;
		opp = dev_pm_opp_find_freq_exact(pci->dev, freq_kbps * width,
						 true);
		if (!IS_ERR(opp)) {
			ret = dev_pm_opp_set_opp(pci->dev, opp);
			if (ret)
				dev_err(pci->dev, "Failed to set OPP for freq (%lu): %d\n",
					freq_kbps * width, ret);
			dev_pm_opp_put(opp);
		}

	}

}
EXPORT_SYMBOL_GPL(qcom_pcie_common_icc_update);
