// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019-2023 MediaTek Inc.
 */

#include "mtk_dp_hdcp2.h"
#include "mtk_dp_reg.h"
#include "ca/tlcDpHdcp.h"
#include "mtk_dp_hdcp.h"
#include "mtk_dp.h"
#include <linux/regmap.h>

#define DPTXHDCPFUNC(fmt, arg...)		\
	pr_info("[DPTXHDCP][%s line:%d]"pr_fmt(fmt), __func__, __LINE__, ##arg)

#define DPTXHDCPMSG(fmt, arg...)                                  \
		pr_info("[DPTXHDCP]"pr_fmt(fmt), ##arg)

u8 t_rtx[HDCP2_RTX_SIZE] = {
	0x18, 0xfa, 0xe4, 0x20, 0x6a, 0xfb, 0x51, 0x49
};

u8 t_tx_caps[HDCP2_TXCAPS_SIZE] = {
	0x02, 0x00, 0x00
};

u8 t_rn[HDCP2_RN_SIZE] = {
	0x32, 0x75, 0x3e, 0xa8, 0x78, 0xa6, 0x38, 0x1c
};

u8 t_riv[HDCP2_RIV_SIZE] = {
	0x40, 0x2b, 0x6b, 0x43, 0xc5, 0xe8, 0x86, 0xd8
};

static u32 get_system_time(void)
{
	u32 tms = (u32)((sched_clock() / 1000000) % 1000000);
	return tms;
}

static u32 get_time_diff(u32 pre_time)
{
	u32 post_time = get_system_time();

	if (pre_time > post_time)
		return ((1000000 - pre_time) + post_time);
	else
		return (post_time - pre_time);
}

static void mhal_dp_tx_hdcp2_fill_stream_type(struct mtk_hdcp_info *hdcp_info, u8 uc_type)
{
	mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_34D0, uc_type, 0xff);
}

static void mdrv_dp_tx_hdcp2_set_state(struct mtk_hdcp_info *hdcp_info, u8 main_state, u8 sub_state)
{
	hdcp_info->hdcp2_info.hdcp_handler.main_state = main_state;
	hdcp_info->hdcp2_info.hdcp_handler.sub_state = sub_state;
}

static void mdrv_dp_tx_hdcp2_set_auth_pass(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	if (enable) {
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_3400, BIT(11), BIT(11));
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_34A4, BIT(4), BIT(4));
	} else {
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_3400, 0, BIT(11));
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_34A4, 0, BIT(4));
	}
}

static void mdrv_dp_tx_hdcp2_enable_auth(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	DPTXHDCPFUNC();
	mdrv_dp_tx_hdcp2_set_auth_pass(hdcp_info, enable);
	if (enable) {
		u32 version = HDCP_V2_3;

		if (hdcp_info->hdcp2_info.hdcp_rx.rx_info[1] & BIT(0))
			version = HDCP_V1;
		else if (hdcp_info->hdcp2_info.hdcp_rx.rx_info[1] & BIT(1))
			version = HDCP_V2;

		tee_hdcp_enable_encrypt(hdcp_info, enable, version);
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_ENC0_P0_3000, BIT(5), BIT(5));
	} else {
		tee_hdcp_enable_encrypt(hdcp_info, enable, HDCP_NONE);
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_ENC0_P0_3000, 0, BIT(5));
	}
}

static int mdrv_dp_tx_hdcp2_init(struct mtk_hdcp_info *hdcp_info)
{
	int err_code = HDCP_ERR_NONE;

	DPTXHDCPFUNC();

	memset(&hdcp_info->hdcp2_info.hdcp_tx, 0, sizeof(struct hdcp2_info_tx));
	memset(&hdcp_info->hdcp2_info.hdcp_rx, 0, sizeof(struct hdcp2_info_rx));
	memcpy(hdcp_info->hdcp2_info.hdcp_tx.rtx, t_rtx, HDCP2_RTX_SIZE);
	memcpy(hdcp_info->hdcp2_info.hdcp_tx.tx_caps, t_tx_caps, HDCP2_TXCAPS_SIZE);
	memcpy(hdcp_info->hdcp2_info.hdcp_tx.rn, t_rn, HDCP2_RN_SIZE);
	memcpy(hdcp_info->hdcp2_info.hdcp_tx.riv, t_riv, HDCP2_RIV_SIZE);

	memset(&hdcp_info->hdcp2_info.hdcp_handler, 0, sizeof(struct hdcp2_handler));
	memset(&hdcp_info->hdcp2_info.stored_pairing_info, 0, sizeof(struct hdcp2_pairing_info));

	mdrv_dp_tx_hdcp2_enable_auth(hdcp_info, false);

	return err_code;
}

static bool mdrv_dp_tx_hdcp2_inc_seq_num_m(struct mtk_hdcp_info *hdcp_info)
{
	u8 i = 0;
	u32 temp_value = 0;

	for (i = 0; i < HDCP2_SEQ_NUM_M_SIZE; i++)
		temp_value |= hdcp_info->hdcp2_info.hdcp_tx.seq_num_m[i] << (i * 8);

	if (temp_value == 0xFFFFFF)
		return false;

	temp_value++;

	for (i = 0; i < HDCP2_SEQ_NUM_M_SIZE; i++)
		hdcp_info->hdcp2_info.hdcp_tx.seq_num_m[i] =
			(temp_value & ((u32)0xFF << (i * 8))) >> (i * 8);
	return true;
}

static bool mdrv_dp_tx_hdcp2_process_rep_auth_stream_manage(struct mtk_hdcp_info *hdcp_info)
{
	bool ret = false;

	hdcp_info->hdcp2_info.hdcp_tx.k[0] = 0x00;
	hdcp_info->hdcp2_info.hdcp_tx.k[1] = 0x01;

	hdcp_info->hdcp2_info.hdcp_tx.stream_id_type[0] = 0x00; //Payload ID
	hdcp_info->hdcp2_info.hdcp_tx.stream_id_type[1] = hdcp_info->hdcp2_info.stream_id_type;

	ret = mdrv_dp_tx_hdcp2_inc_seq_num_m(hdcp_info);

	return ret;
}

static bool mdrv_dp_tx_hdcp2_recv_rep_auth_send_recv_id_list(struct mtk_hdcp_info *hdcp_info)
{
	bool ret = false;
	u8 *buffer = NULL;
	u32 len = 0, len_recv_id_list = 0;
	int rc = 0;

	len_recv_id_list =
		hdcp_info->hdcp2_info.device_count * HDCP2_RECVID_SIZE;
	len = len_recv_id_list + HDCP2_RXINFO_SIZE + HDCP2_SEQ_NUM_V_SIZE;
	buffer = kmalloc(len, GFP_KERNEL);
	if (!buffer) {
		pr_err("Out of Memory\n");
		return ret;
	}

	memcpy(buffer, hdcp_info->hdcp2_info.hdcp_rx.recv_id_list, len_recv_id_list);
	memcpy(buffer + len_recv_id_list, hdcp_info->hdcp2_info.hdcp_rx.rx_info, HDCP2_RXINFO_SIZE);
	memcpy(buffer + len_recv_id_list + HDCP2_RXINFO_SIZE,
	       hdcp_info->hdcp2_info.hdcp_rx.seq_num_v, HDCP2_SEQ_NUM_V_SIZE);

	rc = tee_hdcp2_compute_compare_v(hdcp_info, buffer, len,
					 hdcp_info->hdcp2_info.hdcp_rx.v_prime,
					 hdcp_info->hdcp2_info.hdcp_tx.v_prime);

	if (rc == RET_COMPARE_PASS) {
		ret = true;
		DPTXHDCPMSG("V' is PASS!!\n");
	} else {
		DPTXHDCPMSG("V' is FAIL!!\n");
	}

	kfree(buffer);
	return ret;
}

static bool mdrv_dp_tx_hdcp2_recv_rep_auth_stream_ready(struct mtk_hdcp_info *hdcp_info)
{
	bool ret = false;
	u8 *buffer = NULL;
	u32 len = 0;
	int temp = 0;

	len = HDCP2_STREAMID_TYPE_SIZE + HDCP2_SEQ_NUM_M_SIZE;
	buffer = kmalloc(len, GFP_KERNEL);
	if (!buffer) {
		pr_err("Out of Memory\n");
		return ret;
	}

	memcpy(buffer, hdcp_info->hdcp2_info.hdcp_tx.stream_id_type, HDCP2_STREAMID_TYPE_SIZE);
	memcpy(buffer + HDCP2_STREAMID_TYPE_SIZE, hdcp_info->hdcp2_info.hdcp_tx.seq_num_m,
	       HDCP2_SEQ_NUM_M_SIZE);
	temp = tee_hdcp2_compute_compare_m(hdcp_info, buffer, len,
					   hdcp_info->hdcp2_info.hdcp_rx.m_prime);

	if (temp == RET_COMPARE_PASS) {
		ret = true;
		DPTXHDCPMSG("M' is PASS!!\n");
	} else {
		DPTXHDCPMSG("M' is FAIL!!\n");
	}

	kfree(buffer);
	return ret;
}

static bool mdrv_dp_tx_hdcp2_check_seq_num_v(struct mtk_hdcp_info *hdcp_info)
{
	if ((hdcp_info->hdcp2_info.hdcp_rx.seq_num_v[0] == 0x00 &&
	     hdcp_info->hdcp2_info.hdcp_rx.seq_num_v[1] == 0x00 &&
			hdcp_info->hdcp2_info.hdcp_rx.seq_num_v[2] == 0x00) &&
		hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt > 0xFFFFFF) {
		DPTXHDCPMSG("SeqNumV Rollover!\n");
		return false;
	}

	if ((hdcp_info->hdcp2_info.hdcp_rx.seq_num_v[0]
		!= (u8)((hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt & 0xFF0000) >> 16)) ||
			(hdcp_info->hdcp2_info.hdcp_rx.seq_num_v[1]
		!= (u8)((hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt & 0x00FF00) >> 8)) ||
			(hdcp_info->hdcp2_info.hdcp_rx.seq_num_v[2]
		!= (u8)((hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt & 0x0000FF)))) {
		DPTXHDCPMSG("Invalid Seq_num_V!\n");
		return false;
	}

	hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt++;
	return true;
}

static void mdrv_dp_tx_hdcp2_err_handle(struct mtk_hdcp_info *hdcp_info, int err_msg, int line)
{
	pr_err("MainState:%d; SubState:%d;\n", hdcp_info->hdcp2_info.hdcp_handler.main_state,
	       hdcp_info->hdcp2_info.hdcp_handler.sub_state);

	switch (err_msg) {
	case HDCP_ERR_UNKNOWN_STATE:
		pr_err("Unknown State, line:%d\n", line);
		mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_H1P1, HDCP2_MSG_AUTH_FAIL);
		break;

	case HDCP_ERR_SEND_MSG_FAIL:
		pr_err("Send Msg Fail, line:%d\n", line);
		mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A0F0, HDCP2_MSG_ZERO);
		break;
	case HDCP_ERR_RESPONSE_TIMEROUT:
		pr_err("Response Timeout, line:%d!\n", line);
		mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A0F0, HDCP2_MSG_ZERO);
		break;

	case HDCP_ERR_PROCESS_FAIL:
		pr_err("Process Fail, line:%d!\n", line);
		mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A0F0, HDCP2_MSG_ZERO);
		break;

	default:
		pr_err("NO ERROR!");
		break;
	}
}

static bool mdrv_dp_tx_hdcp2_read_msg(struct mtk_hdcp_info *hdcp_info, u8 cmd_ID)
{
	bool ret = false;
	u8 size = 0;

	switch (cmd_ID) {
	case HDCP2_MSG_AKE_SEND_CERT:
		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_CERT_RX_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.cert, HDCP2_CERTRX_SIZE);
		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_RRX_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.rrx, HDCP2_RRX_SIZE);
		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_RX_CAPS_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.rx_caps, HDCP2_RXCAPS_SIZE);

		hdcp_info->hdcp2_info.read_certrx = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_AKE_SEND_CERT\n");
		break;

	case HDCP2_MSG_AKE_SEND_H_PRIME:
		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_HPRIME_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.h_prime, HDCP2_HPRIME_SIZE);

		hdcp_info->hdcp2_info.read_h_prime = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;

		DPTXHDCPMSG("HDCP2_MSG_AKE_SEND_H_PRIME\n");
		break;

	case HDCP2_MSG_AKE_SEND_PAIRING_INFO:
		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_EKH_KM_RD_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.ekh_km, HDCP2_EKHKM_SIZE);
		hdcp_info->hdcp2_info.read_pairing = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_AKE_SEND_PAIRING_INFO\n");
		break;

	case HDCP2_MSG_LC_SEND_L_PRIME:
		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_LPRIME_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.l_prime, HDCP2_LPRIME_SIZE);

		hdcp_info->hdcp2_info.read_l_prime = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_LC_SEND_L_PRIME\n");
		break;

	case HDCP2_MSG_REPAUTH_SEND_RECVID_LIST:
		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_RXINFO_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.rx_info, HDCP2_RXINFO_SIZE);
		hdcp_info->hdcp2_info.device_count =
			((hdcp_info->hdcp2_info.hdcp_rx.rx_info[1] & 0xf0) >> 4)
				| ((hdcp_info->hdcp2_info.hdcp_rx.rx_info[0] & BIT(0)) << 4);

		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_SEQ_NUM_V_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.seq_num_v, HDCP2_SEQ_NUM_V_SIZE);
		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_VPRIME_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.v_prime, HDCP2_VPRIME_SIZE);
		drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_RECV_ID_LIST_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.recv_id_list,
			hdcp_info->hdcp2_info.device_count
				* HDCP2_RECVID_SIZE);

		hdcp_info->hdcp2_info.read_v_prime = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_REPAUTH_SEND_RECVID_LIST\n");
		break;

	case HDCP2_MSG_REPAUTH_STREAM_READY:
		size = drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_MPRIME_OFFSET,
					hdcp_info->hdcp2_info.hdcp_rx.m_prime,
					HDCP2_REP_MPRIME_SIZE);

		if (size == HDCP2_REP_MPRIME_SIZE)
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_REPAUTH_STREAM_READY\n");
		break;

	default:
		DPTXHDCPMSG("Invalid DPTX_HDCP2_OffSETADDR_ReadMessage !\n");
		break;
	}

	return ret;
}

static bool mdrv_dp_tx_hdcp2_write_msg(struct mtk_hdcp_info *hdcp_info, u8 cmd_ID)
{
	bool ret = false;

	switch (cmd_ID) {
	case HDCP2_MSG_AKE_INIT:
		tee_hdcp2_soft_rst(hdcp_info);
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_RTX_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.rtx, HDCP2_RTX_SIZE);
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_TXCAPS_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.tx_caps, HDCP2_TXCAPS_SIZE);

		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_AKE_Init !\n");
		break;

	case HDCP2_MSG_AKE_NO_STORED_KM:
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_EKPUB_KM_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.ekpub_km, HDCP2_EKPUBKM_SIZE);

		ret = true;

		DPTXHDCPMSG("HDCP2_MSG_AKE_NO_STORED_KM !\n");
		break;

	case HDCP2_MSG_AKE_STORED_KM:
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_EKH_KM_WR_OFFSET,
				  hdcp_info->hdcp2_info.stored_pairing_info.ekh_km,
				  HDCP2_EKHKM_SIZE);
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_M_OFFSET,
				  hdcp_info->hdcp2_info.stored_pairing_info.m, HDCP2_M_SIZE);

		ret = true;

		DPTXHDCPMSG("DPTX_HDCP2_MSG_AKE_STORED_KM !\n");
		break;

	case HDCP2_MSG_LC_INIT:
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_RN_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.rn, HDCP2_RN_SIZE);

		hdcp_info->hdcp2_info.read_l_prime = true;
		ret = true;

		DPTXHDCPMSG("HDCP2_MSG_LC_INIT !\n");
		break;

	case HDCP2_MSG_SKE_SEND_EKS:
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_EDKEY_KS_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.eks, HDCP2_EDKEYKS_SIZE);
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_RIV_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.riv, HDCP2_RIV_SIZE);

		hdcp_info->hdcp2_info.ks_exchange_done = true;

		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_SKE_SEND_EKS !\n");
		break;

	case HDCP2_MSG_REPAUTH_SEND_ACK:
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_V_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.v_prime, HDCP2_VPRIME_SIZE);

		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_SEND_ACK !\n");
		break;

	case HDCP2_MSG_REPAUTH_STREAM_MANAGE:
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_SEQ_NUM_M_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.seq_num_m, HDCP2_SEQ_NUM_M_SIZE);
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_K_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.k, HDCP2_K_SIZE);
		drm_dp_dpcd_write(hdcp_info->aux, DP_HDCP_2_2_REG_STREAM_ID_TYPE_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.stream_id_type,
				  HDCP2_STREAMID_TYPE_SIZE);

		mhal_dp_tx_hdcp2_fill_stream_type(hdcp_info,
						  hdcp_info->hdcp2_info.stream_id_type);

		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_STREAM_MANAGE !\n");
		break;

	default:
		DPTXHDCPMSG("Invalid HDCP2_OffSETADDR_WriteMessage !\n");
		break;
	}

	return ret;
}

static void mdrv_dp_tx_hdcp2_rest_variable(struct mtk_hdcp_info *hdcp_info)
{
	hdcp_info->hdcp2_info.read_certrx = false;
	hdcp_info->hdcp2_info.read_h_prime = false;
	hdcp_info->hdcp2_info.read_pairing = false;
	hdcp_info->hdcp2_info.read_l_prime = false;
	hdcp_info->hdcp2_info.ks_exchange_done = false;
	hdcp_info->hdcp2_info.read_v_prime = false;
}

int mdrv_dp_tx_hdcp2_fsm(struct mtk_hdcp_info *hdcp_info)
{
	static u32 timeout_value;
	static u8 pre_main;
	static u8 pre_sub;
	static u32 pre_time;
	int err_code = HDCP_ERR_NONE;
	bool stored = false;
	u32 time;
	int ret = 0;
	bool tmp = false;

	if (pre_main != hdcp_info->hdcp2_info.hdcp_handler.main_state ||
	    hdcp_info->hdcp2_info.hdcp_handler.sub_state != pre_sub) {
		DPTXHDCPMSG("Port(M : S)= (%d, %d)", hdcp_info->hdcp2_info.hdcp_handler.main_state,
			    hdcp_info->hdcp2_info.hdcp_handler.sub_state);
		pre_main = hdcp_info->hdcp2_info.hdcp_handler.main_state;
		pre_sub = hdcp_info->hdcp2_info.hdcp_handler.sub_state;
	}

	switch (hdcp_info->hdcp2_info.hdcp_handler.main_state) {
	case HDCP2_MS_H1P1:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_ZERO:
			break;
		case HDCP2_MSG_AUTH_FAIL:
			pr_err("HDCP2.x Authentication Fail\n");
			mdrv_dp_tx_hdcp2_enable_auth(hdcp_info, false);
			hdcp_info->auth_status = AUTH_FAIL;
			break;
		}
		break;
	case HDCP2_MS_A0F0:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_ZERO:
			if (hdcp_info->hdcp2_info.enable) {
				mdrv_dp_tx_hdcp2_init(hdcp_info);
				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1,
							   HDCP2_MSG_ZERO);
				DPTXHDCPMSG("Sink Support Hdcp2x!\n");
			} else {
				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_H1P1,
							   HDCP2_MSG_AUTH_FAIL);
				DPTXHDCPMSG("Sink Doesn't Support Hdcp2x!\n");
			}
			break;
		}
		break;

	case HDCP2_MS_A1F1:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_ZERO:
			if (hdcp_info->hdcp2_info.retry_count
				< HDCP2_TX_RETRY_CNT) {
				hdcp_info->hdcp2_info.retry_count++;
				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1,
							   HDCP2_MSG_AKE_INIT);
			} else {
				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_H1P1,
							   HDCP2_MSG_AUTH_FAIL);
				pr_err("Try Max Count\n");
			}
			break;

		case HDCP2_MSG_AKE_INIT:
			tmp = mdrv_dp_tx_hdcp2_write_msg(hdcp_info, HDCP2_MSG_AKE_INIT);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}
			mdrv_dp_tx_hdcp2_rest_variable(hdcp_info);
			hdcp_info->hdcp2_info.read_certrx = true;

			hdcp_info->hdcp2_info.hdcp_handler.send_ake_init = true;
			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1,
						   HDCP2_MSG_AKE_SEND_CERT);
			pre_time = get_system_time();
			break;

		case HDCP2_MSG_AKE_SEND_CERT:
			time = get_time_diff(pre_time);
			if (time < HDCP2_AKESENDCERT_WDT) {
				msleep(20);
				break;
			}
			if (hdcp_info->hdcp2_info.read_certrx)
				mdrv_dp_tx_hdcp2_read_msg(hdcp_info, HDCP2_MSG_AKE_SEND_CERT);

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			ret = tee_ake_certificate(hdcp_info, hdcp_info->hdcp2_info.hdcp_rx.cert,
						  &stored,
				hdcp_info->hdcp2_info.stored_pairing_info.m,
				hdcp_info->hdcp2_info.stored_pairing_info.ekh_km);

			if (ret != RET_COMPARE_PASS) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			hdcp_info->hdcp2_info.hdcp_handler.stored_km = stored;
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1,
						   hdcp_info->hdcp2_info.hdcp_handler.stored_km ?
					HDCP2_MSG_AKE_STORED_KM :
					HDCP2_MSG_AKE_NO_STORED_KM);
			break;

		case HDCP2_MSG_AKE_NO_STORED_KM:
			DPTXHDCPMSG("4. Get Km, derive Ekpub(km)\n");

			tee_enc_rsaes_oaep(hdcp_info, hdcp_info->hdcp2_info.hdcp_tx.ekpub_km);
			/* Prepare ekpub_km to send */
			tmp = mdrv_dp_tx_hdcp2_write_msg(hdcp_info,
							 HDCP2_MSG_AKE_NO_STORED_KM);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1,
						   HDCP2_MSG_AKE_SEND_H_PRIME);
			timeout_value = HDCP2_AKESENDHPRIME_NO_STORED_WDT;
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			pre_time = get_system_time();
			break;
		case HDCP2_MSG_AKE_STORED_KM:
			/* Prepare ekh_km & M to send */
			tmp = mdrv_dp_tx_hdcp2_write_msg(hdcp_info, HDCP2_MSG_AKE_STORED_KM);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			err_code = HDCP_ERR_NONE;
			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1,
						   HDCP2_MSG_AKE_SEND_H_PRIME);
			timeout_value = HDCP2_AKESENDHPRIME_STORED_WDT;
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			pre_time = get_system_time();
			break;

		case HDCP2_MSG_AKE_SEND_H_PRIME:
			if (hdcp_info->hdcp2_info.read_h_prime) {
				mdrv_dp_tx_hdcp2_read_msg(hdcp_info,
							  HDCP2_MSG_AKE_SEND_H_PRIME);
				}
			time = get_time_diff(pre_time);
			if (time > timeout_value) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			ret = tee_ake_h_prime(hdcp_info, hdcp_info->hdcp2_info.hdcp_tx.rtx,
					      hdcp_info->hdcp2_info.hdcp_rx.rrx,
				hdcp_info->hdcp2_info.hdcp_rx.rx_caps,
				hdcp_info->hdcp2_info.hdcp_tx.tx_caps,
				hdcp_info->hdcp2_info.hdcp_rx.h_prime,
				HDCP2_HPRIME_SIZE);
			if (ret != RET_COMPARE_PASS) {
				if (hdcp_info->hdcp2_info.hdcp_handler.stored_km)
					tee_clear_paring(hdcp_info);
				err_code = HDCP_ERR_PROCESS_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (hdcp_info->hdcp2_info.hdcp_handler.stored_km)
				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A2F2,
							   HDCP2_MSG_LC_INIT);
			else
				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1,
							   HDCP2_MSG_AKE_SEND_PAIRING_INFO);

			pre_time = get_system_time();
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			break;

		case HDCP2_MSG_AKE_SEND_PAIRING_INFO:
			if (hdcp_info->hdcp2_info.read_pairing)
				mdrv_dp_tx_hdcp2_read_msg(hdcp_info,
							  HDCP2_MSG_AKE_SEND_PAIRING_INFO);

			/* Ekh_Km must be available less than 200ms, Give mode time for some Rx */
			time = get_time_diff(pre_time);
			if (time >	HDCP2_AKESENDPAIRINGINFO_WDT * 2) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			/* Store m, km, Ekh(km) */
			tee_ake_paring(hdcp_info, hdcp_info->hdcp2_info.hdcp_rx.ekh_km);

			hdcp_info->hdcp2_info.hdcp_handler.send_pair = true;
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A2F2, HDCP2_MSG_LC_INIT);
			pre_time = get_system_time();
			break;
		}
		break;

	case HDCP2_MS_A2F2:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_LC_INIT:
			/* prepare Rn to send */
			tmp = mdrv_dp_tx_hdcp2_write_msg(hdcp_info, HDCP2_MSG_LC_INIT);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}
			hdcp_info->hdcp2_info.hdcp_handler.send_lc_init = true;

			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A2F2,
						   HDCP2_MSG_LC_SEND_L_PRIME);
			pre_time = get_system_time();
			break;

		case HDCP2_MSG_LC_SEND_L_PRIME:
			time = get_time_diff(pre_time);
			if (time < HDCP2_LCSENDLPRIME_WDT)
				break;

			if (hdcp_info->hdcp2_info.read_l_prime)
				mdrv_dp_tx_hdcp2_read_msg(hdcp_info,
							  HDCP2_MSG_LC_SEND_L_PRIME);

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			ret = tee_lc_l_prime(hdcp_info, hdcp_info->hdcp2_info.hdcp_tx.rn,
					     hdcp_info->hdcp2_info.hdcp_rx.l_prime,
				HDCP2_LPRIME_SIZE);
			if (ret != RET_COMPARE_PASS) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			DPTXHDCPMSG("L' is PASS!!\n");
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A3F3, HDCP2_MSG_ZERO);
			pre_time = get_system_time();
			break;
		}
		break;

	case HDCP2_MS_A3F3:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_ZERO:
			tee_ske_enc_ks(hdcp_info, hdcp_info->hdcp2_info.hdcp_tx.riv,
				       hdcp_info->hdcp2_info.hdcp_tx.eks);

			tmp = mdrv_dp_tx_hdcp2_write_msg(hdcp_info, HDCP2_MSG_SKE_SEND_EKS);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A3F3,
						   HDCP2_MSG_SKE_SEND_EKS);
			pre_time = get_system_time();
			break;

		case HDCP2_MSG_SKE_SEND_EKS:
			time = get_time_diff(pre_time);
			if (time >= HDCP2_ENC_EN_TIMER) {
				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A4F4,
							   HDCP2_MSG_ZERO);
			}
			break;
		}
		break;

	case HDCP2_MS_A4F4:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_ZERO:
			if (!hdcp_info->hdcp2_info.repeater) {
				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A5F5,
							   HDCP2_MSG_AUTH_DONE);
			} else {
				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A6F6,
							   HDCP2_MSG_REPAUTH_SEND_RECVID_LIST);
				hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
				pre_time = get_system_time();
			}
			break;
		}
		break;

	case HDCP2_MS_A5F5:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_ZERO:
			break;
		case HDCP2_MSG_AUTH_DONE:
			DPTXHDCPMSG("HDCP2.x Authentication done.\n");
			hdcp_info->auth_status = AUTH_PASS;
			hdcp_info->hdcp2_info.retry_count = 0;
			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A5F5, HDCP2_MSG_ZERO);
			mdrv_dp_tx_hdcp2_enable_auth(hdcp_info, true);
			break;
		}
		break;
	case HDCP2_MS_A6F6:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_REPAUTH_SEND_RECVID_LIST:
			if (hdcp_info->hdcp2_info.read_v_prime)
				mdrv_dp_tx_hdcp2_read_msg(hdcp_info,
							  HDCP2_MSG_REPAUTH_SEND_RECVID_LIST);

			time = get_time_diff(pre_time);
			if (time > HDCP2_REPAUTHSENDRECVID_WDT) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			pre_time = get_system_time();
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A7F7,
						   HDCP2_MSG_REPAUTH_VERIFY_RECVID_LIST);
			break;
		}
		break;

	case HDCP2_MS_A7F7:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_REPAUTH_VERIFY_RECVID_LIST:
			if ((hdcp_info->hdcp2_info.hdcp_rx.rx_info[1] & (BIT(2) | BIT(3))) != 0) {
				pr_err("DEVS_EXCEEDED or CASCADE_EXCEDDED!\n");
				err_code = HDCP_ERR_PROCESS_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			/* check seqNumV here */
			tmp = mdrv_dp_tx_hdcp2_check_seq_num_v(hdcp_info);
			if (!tmp) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			tmp = mdrv_dp_tx_hdcp2_recv_rep_auth_send_recv_id_list(hdcp_info);
			if (!tmp) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A8F8,
						   HDCP2_MSG_REPAUTH_SEND_ACK);
			break;
		}
		break;

	case HDCP2_MS_A8F8:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_REPAUTH_SEND_ACK:
			tmp = mdrv_dp_tx_hdcp2_write_msg(hdcp_info,
							 HDCP2_MSG_REPAUTH_SEND_ACK);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			time = get_time_diff(pre_time);
			if (time > HDCP2_REP_SEND_ACK) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A9F9,
						   HDCP2_MSG_REPAUTH_STREAM_MANAGE);
			hdcp_info->hdcp2_info.hdcp_handler.retry_cnt = 0;
			break;
		}
		break;

	case HDCP2_MS_A9F9:
		switch (hdcp_info->hdcp2_info.hdcp_handler.sub_state) {
		case HDCP2_MSG_REPAUTH_STREAM_MANAGE:
			tmp = mdrv_dp_tx_hdcp2_process_rep_auth_stream_manage(hdcp_info);
			if (!tmp) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			tmp = mdrv_dp_tx_hdcp2_write_msg(hdcp_info,
							 HDCP2_MSG_REPAUTH_STREAM_MANAGE);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			pre_time = get_system_time();
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A9F9,
						   HDCP2_MSG_REPAUTH_STREAM_READY);
			break;
		case HDCP2_MSG_REPAUTH_STREAM_READY:
			time = get_time_diff(pre_time);
			if (time > HDCP2_REPAUTHSTREAMRDY_WDT / 2)
				mdrv_dp_tx_hdcp2_read_msg(hdcp_info,
							  HDCP2_MSG_REPAUTH_STREAM_READY);
			else
				break;

			time = get_time_diff(pre_time);
			if (time > HDCP2_REPAUTHSTREAMRDY_WDT) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			} else if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg) {
				if (hdcp_info->hdcp2_info.hdcp_handler.retry_cnt
					>= HDCP2_STREAM_MANAGE_RETRY_CNT) {
					err_code = HDCP_ERR_RESPONSE_TIMEROUT;
					mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
					break;
				}

				hdcp_info->hdcp2_info.hdcp_handler.retry_cnt++;

				mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A9F9,
							   HDCP2_MSG_REPAUTH_STREAM_READY);
				break;
			}

			tmp = mdrv_dp_tx_hdcp2_recv_rep_auth_stream_ready(hdcp_info);
			if (!tmp) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A5F5,
						   HDCP2_MSG_AUTH_DONE);
			break;
		}
		break;
	default:
		err_code = HDCP_ERR_UNKNOWN_STATE;
		mdrv_dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
		break;
	}

	return err_code;
}

void mdrv_dp_tx_hdcp2_set_start_auth(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	hdcp_info->hdcp2_info.enable = enable;

	if (enable) {
		hdcp_info->auth_status = AUTH_INIT;
		mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A0F0, HDCP2_MSG_ZERO);
	} else {
		hdcp_info->auth_status = AUTH_ZERO;
		mdrv_dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_H1P1, HDCP2_MSG_ZERO);
		mdrv_dp_tx_hdcp2_enable_auth(hdcp_info, false);
	}

	hdcp_info->hdcp2_info.retry_count = 0;
}

bool mdrv_dp_tx_hdcp2_support(struct mtk_hdcp_info *hdcp_info)
{
	u8 temp_buffer[3];
	int ret;

	drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_RX_CAPS_OFFSET, temp_buffer, 0x3);

	if ((temp_buffer[2] & BIT(1)) && temp_buffer[0] == 0x02) {
		hdcp_info->hdcp2_info.enable = true;
		hdcp_info->hdcp2_info.repeater = temp_buffer[2] & BIT(0);
	} else {
		hdcp_info->hdcp2_info.enable = false;
	}

	DPTXHDCPMSG("HDCP.2x CAPABLE: %d, Reapeater: %d\n",
		    hdcp_info->hdcp2_info.enable,
		hdcp_info->hdcp2_info.repeater);

	if (!hdcp_info->hdcp2_info.enable)
		return false;

	ret = tee_add_device(hdcp_info, HDCP_VERSION_2X);
	if (ret != RET_SUCCESS) {
		pr_err("HDCP TA has some error\n");
		hdcp_info->hdcp2_info.enable = false;
	}

	return hdcp_info->hdcp2_info.enable;
}

bool mdrv_dp_tx_hdcp2_irq(struct mtk_hdcp_info *hdcp_info)
{
	u8 rx_status = 0;

	drm_dp_dpcd_read(hdcp_info->aux, DP_HDCP_2_2_REG_RXSTATUS_OFFSET, &rx_status,
			 HDCP2_RXSTATUS_SIZE);

	if (rx_status & BIT(0)) {
		DPTXHDCPMSG("READY_BIT0 Ready!\n");
		hdcp_info->hdcp2_info.read_v_prime = true;
	}

	if (rx_status & BIT(1)) {
		DPTXHDCPMSG("H'_AVAILABLE Ready!\n");
		hdcp_info->hdcp2_info.read_h_prime = true;
	}

	if (rx_status & BIT(2)) {
		DPTXHDCPMSG("PAIRING_AVAILABLE Ready!\n");
		hdcp_info->hdcp2_info.read_pairing = true;
	}

	if (rx_status & BIT(4) || rx_status & BIT(3)) {
		DPTXHDCPMSG("Re-Auth HDCP2X!\n");
		mdrv_dp_tx_hdcp2_set_start_auth(hdcp_info, true);
		mtk_dp_re_authentication(hdcp_info);
	}

	return true;
}

