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
