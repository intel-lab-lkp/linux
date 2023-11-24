/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2023 MediaTek Inc.
 */

#ifndef _MTK_dp_HDCP2_H_
#define _MTK_dp_HDCP2_H_

#include "mtk_dp_hdcp.h"

/* Timeout relative */
#define HDCP2_AKESENDCERT_WDT               100      // 100ms
#define HDCP2_AKESENDHPRIME_NO_STORED_WDT   1000     // 1sec
#define HDCP2_AKESENDHPRIME_STORED_WDT      200      // 200ms
#define HDCP2_AKESENDPAIRINGINFO_WDT        200      // 200ms
#define HDCP2_LCSENDLPRIME_WDT              7        // 7ms
#define HDCP2_ENC_EN_TIMER                  200      // 200 ms
#define HDCP2_REPAUTHSENDRECVID_WDT         3000     // 3 sec
#define HDCP2_REP_SEND_ACK                  2000     // 2 Sec
#define HDCP2_REPAUTHSTREAMRDY_WDT          100      // 100 ms

/* Patch for QD980 LLCTS */
#define HDCP2_TX_RETRY_CNT      3
#define HDCP2_TX_LC_RETRY_CNT   1023
#define HDCP2_STREAM_MANAGE_RETRY_CNT   8

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

enum ENUM_HDCP2_MSG_LIST {
	HDCP2_MSG_ZERO                            = 0,
	HDCP2_MSG_AKE_INIT                        = 1,
	HDCP2_MSG_AKE_SEND_CERT                   = 2,
	HDCP2_MSG_AKE_NO_STORED_KM                = 3,
	HDCP2_MSG_AKE_STORED_KM                   = 4,
	HDCP2_MSG_AKE_SEND_H_PRIME                = 5,
	HDCP2_MSG_AKE_SEND_PAIRING_INFO           = 6,
	HDCP2_MSG_LC_INIT                         = 7,
	HDCP2_MSG_LC_SEND_L_PRIME                 = 8,
	HDCP2_MSG_SKE_SEND_EKS                    = 9,
	HDCP2_MSG_REPAUTH_SEND_RECVID_LIST        = 10,
	HDCP2_MSG_REPAUTH_VERIFY_RECVID_LIST      = 11,
	HDCP2_MSG_REPAUTH_SEND_ACK                = 12,
	HDCP2_MSG_REPAUTH_STREAM_MANAGE           = 13,
	HDCP2_MSG_REPAUTH_STREAM_READY            = 14,
	HDCP2_MSG_AUTH_FAIL	                  = 15,
	HDCP2_MSG_AUTH_DONE	                  = 16,
};

enum ENUM_HDCP_ERR_CODE {
	HDCP_ERR_NONE = 0,
	HDCP_ERR_UNKNOWN_STATE,
	HDCP_ERR_SEND_MSG_FAIL,
	HDCP_ERR_RESPONSE_TIMEROUT,
	HDCP_ERR_PROCESS_FAIL
};

int mdrv_dp_tx_hdcp2_fsm(struct mtk_hdcp_info *hdcp_info);
void mdrv_dp_tx_hdcp2_set_start_auth(struct mtk_hdcp_info *hdcp_info, bool enable);
bool mdrv_dp_tx_hdcp2_support(struct mtk_hdcp_info *hdcp_info);
bool mdrv_dp_tx_hdcp2_irq(struct mtk_hdcp_info *hdcp_info);

#endif /* _MTK_dp_HDCP2_H_ */

