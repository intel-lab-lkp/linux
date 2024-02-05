/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#ifndef _MTK_dp_HDCP2_H_
#define _MTK_dp_HDCP2_H_

#include "tlc_dp_hdcp.h"

/* Timeout relative */
#define HDCP2_REP_SEND_ACK 2000

/* Patch for QD980 LLCTS */
#define HDCP2_TX_RETRY_CNT 3
#define HDCP2_TX_LC_RETRY_CNT 1023
#define HDCP2_STREAM_MANAGE_RETRY_CNT 8

/* HDCP2.2 Msg IDs */
#define HDCP_2_2_STREAM_TYPE 20
#define HDCP_2_2_REP_VERIFY_RECVID_LIST 21
#define HDCP_2_2_AUTH_FAIL 22
#define HDCP_2_2_AUTH_DONE 23

enum ENUM_HDCP2TX_MAIN_STATE {
	HDCP2_MS_H1P1 = 0,
	HDCP2_MS_A0F0 = 1,
	HDCP2_MS_A1F1 = 2,
	HDCP2_MS_A2F2 = 3,
	HDCP2_MS_A3F3 = 4,
	HDCP2_MS_A4F4 = 5,
	HDCP2_MS_A5F5 = 6,
	HDCP2_MS_A6F6 = 7,
	HDCP2_MS_A7F7 = 8,
	HDCP2_MS_A8F8 = 9,
	HDCP2_MS_A9F9 = 10
};

enum ENUM_HDCP_ERR_CODE {
	HDCP_ERR_NONE = 0,
	HDCP_ERR_UNKNOWN_STATE,
	HDCP_ERR_SEND_MSG_FAIL,
	HDCP_ERR_RESPONSE_TIMEROUT,
	HDCP_ERR_PROCESS_FAIL
};

int dp_tx_hdcp2_fsm(struct mtk_hdcp_info *hdcp_info);
void dp_tx_hdcp2_set_start_auth(struct mtk_hdcp_info *hdcp_info, bool enable);
bool dp_tx_hdcp2_support(struct mtk_hdcp_info *hdcp_info);
bool dp_tx_hdcp2_irq(struct mtk_hdcp_info *hdcp_info);

#endif /* _MTK_dp_HDCP2_H_ */
