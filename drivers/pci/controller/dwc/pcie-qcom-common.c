// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014-2015, 2020 The Linux Foundation. All rights reserved.
 * Copyright (c) 2015, 2021 Linaro Limited.
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */

#include <linux/pci.h>
#include <linux/interconnect.h>

#include "../../pci.h"
#include "pcie-designware.h"
#include "pcie-qcom-common.h"

#define QCOM_PCIE_LINK_SPEED_TO_BW(speed) \
		Mbps_to_icc(PCIE_SPEED2MBS_ENC(pcie_link_speed[speed]))

void qcom_pcie_common_set_16gt_eq_settings(struct dw_pcie *pci)
{
	u32 reg;

	/*
	 * GEN3_RELATED_OFF is repurposed to be used with GEN4(16GT/s) rate
	 * as well based on RATE_SHADOW_SEL_MASK settings on this register.
	 */
	reg = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
	reg &= ~GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL;
	reg &= ~GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK;
	reg |= (0x1 << GEN3_RELATED_OFF_RATE_SHADOW_SEL_SHIFT);
	dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, reg);

	reg = dw_pcie_readl_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF);
	reg = GEN3_EQ_FMDC_T_MIN_PHASE23(0) |
		GEN3_EQ_FMDC_N_EVALS(0xD) |
		GEN3_EQ_FMDC_MAX_PRE_CUSROR_DELTA(0x5) |
		GEN3_EQ_FMDC_MAX_POST_CUSROR_DELTA(0x5);
	dw_pcie_writel_dbi(pci, GEN3_EQ_FB_MODE_DIR_CHANGE_OFF, reg);

	reg = dw_pcie_readl_dbi(pci, GEN3_EQ_CONTROL_OFF);
	reg = GEN3_EQ_CONTROL_OFF_FB_MODE(0) |
		GEN3_EQ_CONTROL_OFF_PHASE23_EXIT_MODE(0) |
		GEN3_EQ_CONTROL_OFF_FOM_INC_INITIAL_EVAL(0) |
		GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC(0);
	dw_pcie_writel_dbi(pci, GEN3_EQ_CONTROL_OFF, reg);
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_set_16gt_eq_settings);

void qcom_pcie_common_set_16gt_rx_margining_settings(struct dw_pcie *pci)
{
	u32 reg;

	reg = dw_pcie_readl_dbi(pci, GEN4_LANE_MARGINING_1_OFF);
	reg = MARGINING_MAX_VOLTAGE_OFFSET(0x24) |
		MARGINING_NUM_VOLTAGE_STEPS(0x78) |
		MARGINING_MAX_TIMING_OFFSET(0x32) |
		MARGINING_NUM_TIMING_STEPS(0x10);
	dw_pcie_writel_dbi(pci, GEN4_LANE_MARGINING_1_OFF, reg);

	reg = dw_pcie_readl_dbi(pci, GEN4_LANE_MARGINING_2_OFF);
	reg = MARGINING_IND_ERROR_SAMPLER(1) |
		MARGINING_SAMPLE_REPORTING_METHOD(1) |
		MARGINING_IND_LEFT_RIGHT_TIMING(1) |
		MARGINING_VOLTAGE_SUPPORTED(1) |
		MARGINING_IND_UP_DOWN_VOLTAGE(0) |
		MARGINING_MAXLANES(pci->num_lanes) |
		MARGINING_SAMPLE_RATE_TIMING(0x3f) |
		MARGINING_SAMPLE_RATE_VOLTAGE(0x3f);
	dw_pcie_writel_dbi(pci, GEN4_LANE_MARGINING_2_OFF, reg);
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_set_16gt_rx_margining_settings);

int qcom_pcie_common_icc_get_resource(struct dw_pcie *pci, struct icc_path **icc_mem_p)
{
	*icc_mem_p = devm_of_icc_get(pci->dev, "pcie-mem");
	if (IS_ERR_OR_NULL(icc_mem_p))
		return PTR_ERR(icc_mem_p);

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_icc_get_resource);

int qcom_pcie_common_icc_init(struct dw_pcie *pci, struct icc_path *icc_mem)
{
	int ret;

	/*
	 * Some Qualcomm platforms require interconnect bandwidth constraints
	 * to be set before enabling interconnect clocks.
	 *
	 * Set an initial peak bandwidth corresponding to single-lane Gen 1
	 * for the pcie-mem path.
	 */
	ret = icc_set_bw(icc_mem, 0, QCOM_PCIE_LINK_SPEED_TO_BW(1));
	if (ret) {
		dev_err(pci->dev, "failed to set interconnect bandwidth: %d\n",
			ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_icc_init);

void qcom_pcie_common_icc_update(struct dw_pcie *pci, struct icc_path *icc_mem)
{
	u32 offset, status;
	int speed, width;
	int ret;

	if (!icc_mem)
		return;

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	status = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA);

	/* Only update constraints if link is up. */
	if (!(status & PCI_EXP_LNKSTA_DLLLA))
		return;

	speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, status);
	width = FIELD_GET(PCI_EXP_LNKSTA_NLW, status);

	ret = icc_set_bw(icc_mem, 0, width * QCOM_PCIE_LINK_SPEED_TO_BW(speed));
	if (ret)
		dev_err(pci->dev, "failed to set interconnect bandwidth: %d\n",
			ret);
}
EXPORT_SYMBOL_GPL(qcom_pcie_common_icc_update);
