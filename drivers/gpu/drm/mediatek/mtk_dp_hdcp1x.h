/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#ifndef _MTK_DP_HDCP1X_H_
#define _MTK_DP_HDCP1X_H_

#include "tlc_dp_hdcp.h"

#define HDCP_VERSION_1X 1

void dp_tx_hdcp1x_get_info(struct mtk_hdcp_info *hdcp_info);
int dp_tx_hdcp1x_enable(struct mtk_hdcp_info *hdcp_info);
int dp_tx_hdcp1x_disabel(struct mtk_hdcp_info *hdcp_info);
int dp_tx_hdcp1x_check_link(struct mtk_hdcp_info *hdcp_info);
#endif /* _MTK_DP_HDCP1X_H_ */
