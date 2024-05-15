/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2015 - 2024 Beijing WangXun Technology Co., Ltd. */

#ifndef _WX_SRIOV_H_
#define _WX_SRIOV_H_

int wx_disable_sriov(struct wx *wx);
int wx_pci_sriov_configure(struct pci_dev *pdev, int num_vfs);

#endif /* _WX_SRIOV_H_ */
