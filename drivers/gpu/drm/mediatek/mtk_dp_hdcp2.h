/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#ifndef _MTK_dp_HDCP2_H_
#define _MTK_dp_HDCP2_H_

#include "tlc_dp_hdcp.h"

#define HDCP_VERSION_2X 2

enum check_link {
	LINK_PROTECTED	= 0,
	TOPOLOGY_CHANGE,
	LINK_INTEGRITY_FAILURE,
	REAUTH_REQUEST
};

void dp_tx_hdcp2x_get_info(struct mtk_hdcp_info *hdcp_info);
int dp_tx_hdcp2x_enable(struct mtk_hdcp_info *hdcp_info);
int dp_tx_hdcp2x_disabel(struct mtk_hdcp_info *hdcp_info);
int dp_tx_hdcp2x_check_link(struct mtk_hdcp_info *hdcp_info);
void dp_tx_hdcp2x_irq(struct mtk_hdcp_info *hdcp_info);
#endif /* _MTK_dp_HDCP2_H_ */
