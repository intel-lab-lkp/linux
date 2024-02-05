/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#ifndef _MTK_DP_HDCP1X_H_
#define _MTK_DP_HDCP1X_H_

#include "tlc_dp_hdcp.h"

#define HDCP1X_BSTATUS_TIMEOUT_CNT              600
#define HDCP1X_R0_WDT                           100
#define HDCP1X_REP_RDY_WDT                      5000

#define HDCP1X_REAUNTH_COUNT          3

enum DPTX_DRV_HDCP1X_main_states {
	HDCP1X_main_state_H2 = 0,
	HDCP1X_main_state_A0 = 1,
	HDCP1X_main_state_A1 = 2,
	HDCP1X_main_state_A2 = 3,
	HDCP1X_main_state_A3 = 4,
	HDCP1X_main_state_A4 = 5,
	HDCP1X_main_state_A5 = 6,
	HDCP1X_main_state_A6 = 7,
	HDCP1X_main_state_A7 = 8,
};

enum DPTX_DRV_HDCP1X_sub_states {
	HDCP1X_sub_FSM_IDLE              = 0,
	HDCP1X_sub_FSM_CHECKHDCPCAPABLE	= 1,
	HDCP1X_sub_FSM_exchange_KSV       = 2,
	HDCP1X_sub_FSM_verify_bksv        = 3,
	HDCP1X_sub_FSM_computation       = 4,
	HDCP1X_sub_FSM_check_R0           = 5,
	HDCP1X_sub_FSM_auth_done          = 6,
	HDCP1X_sub_FSM_polling_rdy_bit     = 7,
	HDCP1X_sub_FSM_auth_with_repeater  = 8,
	HDCP1X_sub_FSM_auth_fail          = 9,
};

bool dp_tx_hdcp1x_support(struct mtk_hdcp_info *hdcp_info);
void dp_tx_hdcp1x_fsm(struct mtk_hdcp_info *hdcp_info);
void dp_tx_hdcp1x_set_start_auth(struct mtk_hdcp_info *hdcp_info, bool enable);

#endif /* _MTK_DP_HDCP1X_H_ */
