/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2015 - 2025 Beijing WangXun Technology Co., Ltd. */

#ifndef _WX_SRIOV_H_
#define _WX_SRIOV_H_

void wx_disable_sriov(struct wx *wx);
int wx_pci_sriov_configure(struct pci_dev *pdev, int num_vfs);
void wx_msg_task(struct wx *wx);
void wx_disable_vf_rx_tx(struct wx *wx);
void wx_ping_all_vfs_with_link_status(struct wx *wx, bool link_up);

#endif /* _WX_SRIOV_H_ */
