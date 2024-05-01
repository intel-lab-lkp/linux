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

struct icc_path *qcom_pcie_common_icc_get_resource(struct dw_pcie *pci, const char *path)
{
	struct icc_path *icc_mem_p;

	icc_mem_p = devm_of_icc_get(pci->dev, path);
	if (IS_ERR_OR_NULL(icc_mem_p))
		return PTR_ERR(icc_mem_p);
	return icc_mem_p;
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
		dev_err(pci->dev, "Failed to set interconnect bandwidth: %d\n",
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
