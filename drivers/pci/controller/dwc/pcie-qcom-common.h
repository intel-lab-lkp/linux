/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2014-2015, 2020 The Linux Foundation. All rights reserved.
 * Copyright (c) 2015, 2021 Linaro Limited.
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "pcie-designware.h"

struct icc_path *qcom_pcie_common_icc_get_resource(struct dw_pcie *pci, const char *path);
int qcom_pcie_common_icc_init(struct dw_pcie *pci, struct icc_path *icc_mem);
void qcom_pcie_common_icc_update(struct dw_pcie *pci, struct icc_path *icc_mem);
void qcom_pcie_common_set_16gt_eq_settings(struct dw_pcie *pci);
void qcom_pcie_common_set_16gt_rx_margining_settings(struct dw_pcie *pci);
