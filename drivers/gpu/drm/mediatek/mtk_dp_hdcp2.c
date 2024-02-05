// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#include "mtk_dp_hdcp2.h"
#include "mtk_dp_reg.h"
#include "mtk_dp.h"

u8 t_rtx[HDCP_2_2_RTX_LEN] = {
	0x18, 0xfa, 0xe4, 0x20, 0x6a, 0xfb, 0x51, 0x49
};

u8 t_tx_caps[HDCP_2_2_TXCAPS_LEN] = {
	0x02, 0x00, 0x00
};

u8 t_rn[HDCP_2_2_RN_LEN] = {
	0x32, 0x75, 0x3e, 0xa8, 0x78, 0xa6, 0x38, 0x1c
};

u8 t_riv[HDCP_2_2_RIV_LEN] = {
	0x40, 0x2b, 0x6b, 0x43, 0xc5, 0xe8, 0x86, 0xd8
};

static void dp_tx_hdcp2_fill_stream_type(struct mtk_hdcp_info *hdcp_info, u8 uc_type)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);

	mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_34D0, uc_type, 0xff);
}

static void dp_tx_hdcp2_set_state(struct mtk_hdcp_info *hdcp_info, u8 main_state, u8 sub_state)
{
	hdcp_info->hdcp2_info.hdcp_handler.main_state = main_state;
	hdcp_info->hdcp2_info.hdcp_handler.sub_state = sub_state;
}

static void dp_tx_hdcp2_set_auth_pass(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);

	if (enable) {
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_3400, BIT(11), BIT(11));
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_34A4, BIT(4), BIT(4));
	} else {
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_3400, 0, BIT(11));
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_34A4, 0, BIT(4));
	}
}

static void dp_tx_hdcp2_enable_auth(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);

	DPTXHDCPFUNC();
	dp_tx_hdcp2_set_auth_pass(hdcp_info, enable);
	if (enable) {
		u32 version = HDCP_V2_3;

		if (hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.rx_info[1] & BIT(0))
			version = HDCP_V1;
		else if (hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.rx_info[1] & BIT(1))
			version = HDCP_V2;

		tee_hdcp_enable_encrypt(hdcp_info, enable, version);
		mtk_dp_update_bits(mtk_dp, MTK_DP_ENC0_P0_3000, BIT(5), BIT(5));
	} else {
		tee_hdcp_enable_encrypt(hdcp_info, enable, HDCP_NONE);
		mtk_dp_update_bits(mtk_dp, MTK_DP_ENC0_P0_3000, 0, BIT(5));
	}
}

static int dp_tx_hdcp2_init(struct mtk_hdcp_info *hdcp_info)
{
	int err_code = HDCP_ERR_NONE;

	DPTXHDCPFUNC();

	memset(&hdcp_info->hdcp2_info.hdcp_tx, 0, sizeof(struct hdcp2_info_tx));
	memset(&hdcp_info->hdcp2_info.hdcp_rx, 0, sizeof(struct hdcp2_info_rx));
	memcpy(hdcp_info->hdcp2_info.hdcp_tx.ake_init.r_tx, t_rtx, HDCP_2_2_RTX_LEN);
	memcpy(&hdcp_info->hdcp2_info.hdcp_tx.tx_caps, t_tx_caps, HDCP_2_2_TXCAPS_LEN);
	memcpy(hdcp_info->hdcp2_info.hdcp_tx.lc_init.r_n, t_rn, HDCP_2_2_RN_LEN);
	memcpy(hdcp_info->hdcp2_info.hdcp_tx.send_eks.riv, t_riv, HDCP_2_2_RIV_LEN);

	memset(&hdcp_info->hdcp2_info.hdcp_handler, 0, sizeof(struct hdcp2_handler));
	memset(&hdcp_info->hdcp2_info.ake_stored_km, 0, sizeof(struct hdcp2_ake_stored_km));

	dp_tx_hdcp2_enable_auth(hdcp_info, false);

	return err_code;
}

static bool dp_tx_hdcp2_inc_seq_num_m(struct mtk_hdcp_info *hdcp_info)
{
	u32 tmp = 0;

	tmp = drm_hdcp_be24_to_cpu(hdcp_info->hdcp2_info.hdcp_tx.stream_manage.seq_num_m);

	if (tmp == 0xFFFFFF)
		return false;

	tmp++;

	drm_hdcp_cpu_to_be24(hdcp_info->hdcp2_info.hdcp_tx.stream_manage.seq_num_m, tmp);
	return true;
}

static bool dp_tx_hdcp2_process_rep_auth_stream_manage(struct mtk_hdcp_info *hdcp_info)
{
	bool ret = false;

	hdcp_info->hdcp2_info.hdcp_tx.k[0] = 0x00;
	hdcp_info->hdcp2_info.hdcp_tx.k[1] = 0x01;

	hdcp_info->hdcp2_info.hdcp_tx.stream_id_type[0] = 0x00; //Payload ID
	hdcp_info->hdcp2_info.hdcp_tx.stream_id_type[1] = hdcp_info->hdcp2_info.stream_id_type;

	ret = dp_tx_hdcp2_inc_seq_num_m(hdcp_info);

	return ret;
}

static bool dp_tx_hdcp2_recv_rep_auth_send_recv_id_list(struct mtk_hdcp_info *hdcp_info)
{
	bool ret = false;
	u8 *buffer = NULL;
	u32 len = 0, len_recv_id_list = 0;
	int rc = 0;

	len_recv_id_list = hdcp_info->hdcp2_info.device_count * HDCP_2_2_RECEIVER_ID_LEN;
	len = len_recv_id_list + HDCP_2_2_RXINFO_LEN + HDCP_2_2_SEQ_NUM_LEN;
	buffer = kmalloc(len, GFP_KERNEL);
	if (!buffer) {
		pr_err("2.x: Out of Memory\n");
		return ret;
	}

	memcpy(buffer, hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.receiver_ids,
	       len_recv_id_list);
	memcpy(buffer + len_recv_id_list,
	       hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.rx_info, HDCP_2_2_RXINFO_LEN);
	memcpy(buffer + len_recv_id_list + HDCP_2_2_RXINFO_LEN,
	       hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.seq_num_v, HDCP_2_2_SEQ_NUM_LEN);

	rc = tee_hdcp2_compute_compare_v(hdcp_info, buffer, len,
					 hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.v_prime,
		hdcp_info->hdcp2_info.hdcp_tx.send_ack.v);

	if (rc == RET_COMPARE_PASS) {
		ret = true;
		DPTXHDCPMSG("2.x: V' is PASS!!\n");
	} else {
		DPTXHDCPMSG("2.x: V' is FAIL!!\n");
	}

	kfree(buffer);
	return ret;
}

static bool dp_tx_hdcp2_recv_rep_auth_stream_ready(struct mtk_hdcp_info *hdcp_info)
{
	bool ret = false;
	u8 *buffer = NULL;
	u32 len = 0;
	int tmp = 0;

	len = HDCP2_STREAMID_TYPE_LEN + HDCP_2_2_SEQ_NUM_LEN;
	buffer = kmalloc(len, GFP_KERNEL);
	if (!buffer) {
		pr_err("2.x: Out of Memory\n");
		return ret;
	}

	memcpy(buffer, hdcp_info->hdcp2_info.hdcp_tx.stream_id_type, HDCP2_STREAMID_TYPE_LEN);
	memcpy(buffer + HDCP2_STREAMID_TYPE_LEN,
	       hdcp_info->hdcp2_info.hdcp_tx.stream_manage.seq_num_m,
	       HDCP_2_2_SEQ_NUM_LEN);
	tmp = tee_hdcp2_compute_compare_m(hdcp_info, buffer, len,
					  hdcp_info->hdcp2_info.hdcp_rx.stream_ready.m_prime);

	if (tmp == RET_COMPARE_PASS) {
		ret = true;
		DPTXHDCPMSG("2.x: M' is PASS!!\n");
	} else {
		DPTXHDCPMSG("2.x: M' is FAIL!!\n");
	}

	kfree(buffer);
	return ret;
}

static bool dp_tx_hdcp2_check_seq_num_v(struct mtk_hdcp_info *hdcp_info)
{
	if ((hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.seq_num_v[0] == 0x00 &&
	     hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.seq_num_v[1] == 0x00 &&
			hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.seq_num_v[2] == 0x00) &&
		hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt > 0xFFFFFF) {
		DPTXHDCPMSG("2.x: SeqNumV Rollover!\n");
		return false;
	}

	if ((hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.seq_num_v[0]
		!= (u8)((hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt & 0xFF0000) >> 16)) ||
			(hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.seq_num_v[1]
		!= (u8)((hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt & 0x00FF00) >> 8)) ||
			(hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.seq_num_v[2]
		!= (u8)((hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt & 0x0000FF)))) {
		DPTXHDCPMSG("2.x: Invalid Seq_num_V!\n");
		return false;
	}

	hdcp_info->hdcp2_info.hdcp_handler.seq_num_v_cnt++;
	return true;
}

static void dp_tx_hdcp2_err_handle(struct mtk_hdcp_info *hdcp_info, int err_msg, int line)
{
	pr_err("2.x: MainState:%d; SubState:%d;\n", hdcp_info->hdcp2_info.hdcp_handler.main_state,
	       hdcp_info->hdcp2_info.hdcp_handler.sub_state);

	switch (err_msg) {
	case HDCP_ERR_UNKNOWN_STATE:
		pr_err("2.x: Unknown State, line:%d\n", line);
		dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_H1P1, HDCP_2_2_AUTH_FAIL);
		break;

	case HDCP_ERR_SEND_MSG_FAIL:
		pr_err("2.x: Send Msg Fail, line:%d\n", line);
		dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A0F0, HDCP_2_2_NULL_MSG);
		break;
	case HDCP_ERR_RESPONSE_TIMEROUT:
		pr_err("2.x: Response Timeout, line:%d!\n", line);
		dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A0F0, HDCP_2_2_NULL_MSG);
		break;

	case HDCP_ERR_PROCESS_FAIL:
		pr_err("2.x: Process Fail, line:%d!\n", line);
		dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A0F0, HDCP_2_2_NULL_MSG);
		break;

	default:
		pr_err("2.x: NO ERROR!");
		break;
	}
}

static bool dp_tx_hdcp2_read_msg(struct mtk_hdcp_info *hdcp_info, u8 cmd_ID)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	bool ret = false;
	u8 size = 0;

	switch (cmd_ID) {
	case HDCP_2_2_AKE_SEND_CERT:
		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_CERT_RX_OFFSET,
				 (void *)&hdcp_info->hdcp2_info.hdcp_rx.cert_rx, HDCP2_CERTRX_LEN);
		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_RRX_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.send_cert.r_rx, HDCP_2_2_RRX_LEN);
		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_RX_CAPS_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.send_cert.rx_caps,
				 HDCP_2_2_RXCAPS_LEN);

		hdcp_info->hdcp2_info.read_certrx = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("2.x: HDCP_2_2_AKE_SEND_CERT\n");
		break;

	case HDCP_2_2_AKE_SEND_HPRIME:
		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_HPRIME_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.send_hprime.h_prime,
				 HDCP_2_2_H_PRIME_LEN);

		hdcp_info->hdcp2_info.read_h_prime = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;

		DPTXHDCPMSG("2.x: HDCP_2_2_AKE_SEND_HPRIME\n");
		break;

	case HDCP_2_2_AKE_SEND_PAIRING_INFO:
		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_EKH_KM_RD_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.pairing_info.e_kh_km,
				 HDCP_2_2_E_KH_KM_LEN);
		hdcp_info->hdcp2_info.read_pairing = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("2.x: HDCP_2_2_AKE_SEND_PAIRING_INFO\n");
		break;

	case HDCP_2_2_LC_SEND_LPRIME:
		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_LPRIME_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.send_lprime.l_prime,
				 HDCP_2_2_L_PRIME_LEN);

		hdcp_info->hdcp2_info.read_l_prime = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("2.x: HDCP_2_2_LC_SEND_LPRIME\n");
		break;

	case HDCP_2_2_REP_SEND_RECVID_LIST:
		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_RXINFO_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.rx_info,
				 HDCP_2_2_RXINFO_LEN);
		hdcp_info->hdcp2_info.device_count =
			((hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.rx_info[1] & 0xf0) >> 4)
				| ((hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.rx_info[0]
				& BIT(0)) << 4);

		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_SEQ_NUM_V_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.seq_num_v,
				 HDCP_2_2_SEQ_NUM_LEN);
		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_VPRIME_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.v_prime,
				 HDCP_2_2_V_PRIME_HALF_LEN);
		drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_RECV_ID_LIST_OFFSET,
				 hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.receiver_ids,
			hdcp_info->hdcp2_info.device_count * HDCP_2_2_RECEIVER_ID_LEN);

		hdcp_info->hdcp2_info.read_v_prime = false;
		hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("2.x: HDCP_2_2_REP_SEND_RECVID_LIST\n");
		break;

	case HDCP_2_2_REP_STREAM_READY:
		size = drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_MPRIME_OFFSET,
					hdcp_info->hdcp2_info.hdcp_rx.stream_ready.m_prime,
					HDCP_2_2_MPRIME_LEN);

		if (size == HDCP_2_2_MPRIME_LEN)
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = true;
		ret = true;
		DPTXHDCPMSG("2.x: HDCP_2_2_REP_STREAM_READY\n");
		break;

	default:
		DPTXHDCPMSG("2.x: Invalid DPTX_HDCP2_OffSETADDR_ReadMessage !\n");
		break;
	}

	return ret;
}

static bool dp_tx_hdcp2_write_msg(struct mtk_hdcp_info *hdcp_info, u8 cmd_ID)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	bool ret = false;

	switch (cmd_ID) {
	case HDCP_2_2_AKE_INIT:
		tee_hdcp2_soft_rst(hdcp_info);
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_RTX_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.ake_init.r_tx, HDCP_2_2_RTX_LEN);
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_TXCAPS_OFFSET,
				  (void *)&hdcp_info->hdcp2_info.hdcp_tx.tx_caps,
				  HDCP_2_2_TXCAPS_LEN);

		ret = true;
		DPTXHDCPMSG("2.x: HDCP_2_2_AKE_Init !\n");
		break;

	case HDCP_2_2_AKE_NO_STORED_KM:
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_EKPUB_KM_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.no_stored_km.e_kpub_km,
				  HDCP_2_2_E_KPUB_KM_LEN);

		ret = true;

		DPTXHDCPMSG("2.x: HDCP_2_2_AKE_NO_STORED_KM !\n");
		break;

	case HDCP_2_2_AKE_STORED_KM:
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_EKH_KM_WR_OFFSET,
				  hdcp_info->hdcp2_info.ake_stored_km.e_kh_km_m,
				  HDCP_2_2_E_KH_KM_LEN);
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_M_OFFSET,
				  hdcp_info->hdcp2_info.ake_stored_km.e_kh_km_m +
				  HDCP_2_2_E_KH_KM_LEN,
				  HDCP_2_2_E_KH_KM_M_LEN - HDCP_2_2_E_KH_KM_LEN);

		ret = true;

		DPTXHDCPMSG("2.x: DPTX_HDCP_2_2_AKE_STORED_KM !\n");
		break;

	case HDCP_2_2_LC_INIT:
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_RN_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.lc_init.r_n, HDCP_2_2_RN_LEN);

		hdcp_info->hdcp2_info.read_l_prime = true;
		ret = true;

		DPTXHDCPMSG("2.x: HDCP_2_2_LC_INIT !\n");
		break;

	case HDCP_2_2_SKE_SEND_EKS:
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_EDKEY_KS_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.send_eks.e_dkey_ks,
				  HDCP_2_2_E_DKEY_KS_LEN);
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_RIV_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.send_eks.riv, HDCP_2_2_RIV_LEN);

		hdcp_info->hdcp2_info.ks_exchange_done = true;

		ret = true;
		DPTXHDCPMSG("2.x: HDCP_2_2_SKE_SEND_EKS !\n");
		break;

	case HDCP_2_2_STREAM_TYPE:
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_STREAM_TYPE_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.stream_id_type, 1);

		ret = true;
		DPTXHDCPMSG("HDCP2_MSG_DP_STREAM_TYPE !\n");
		break;

	case HDCP_2_2_REP_SEND_ACK:
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_V_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.send_ack.v,
				  HDCP_2_2_V_PRIME_HALF_LEN);

		ret = true;
		DPTXHDCPMSG("2.x: HDCP_2_2_SEND_ACK !\n");
		break;

	case HDCP_2_2_REP_STREAM_MANAGE:
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_SEQ_NUM_M_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.stream_manage.seq_num_m,
				  HDCP_2_2_SEQ_NUM_LEN);
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_K_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.k, HDCP2_K_LEN);
		drm_dp_dpcd_write(&mtk_dp->aux, DP_HDCP_2_2_REG_STREAM_ID_TYPE_OFFSET,
				  hdcp_info->hdcp2_info.hdcp_tx.stream_id_type,
				  HDCP2_STREAMID_TYPE_LEN);

		dp_tx_hdcp2_fill_stream_type(hdcp_info, hdcp_info->hdcp2_info.stream_id_type);

		ret = true;
		DPTXHDCPMSG("2.x: HDCP_2_2_STREAM_MANAGE !\n");
		break;

	default:
		DPTXHDCPMSG("2.x: Invalid HDCP2_OffSETADDR_WriteMessage !\n");
		break;
	}

	return ret;
}

static void dp_tx_hdcp2_rest_variable(struct mtk_hdcp_info *hdcp_info)
{
	hdcp_info->hdcp2_info.read_certrx = false;
	hdcp_info->hdcp2_info.read_h_prime = false;
	hdcp_info->hdcp2_info.read_pairing = false;
	hdcp_info->hdcp2_info.read_l_prime = false;
	hdcp_info->hdcp2_info.ks_exchange_done = false;
	hdcp_info->hdcp2_info.read_v_prime = false;
}

int dp_tx_hdcp2_fsm(struct mtk_hdcp_info *hdcp_info)
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
		DPTXHDCPMSG("2.x: Port(M : S)= (%d, %d)",
			    hdcp_info->hdcp2_info.hdcp_handler.main_state,
			    hdcp_info->hdcp2_info.hdcp_handler.sub_state);
		pre_main = hdcp_info->hdcp2_info.hdcp_handler.main_state;
		pre_sub = hdcp_info->hdcp2_info.hdcp_handler.sub_state;
	}

	switch (hdcp_info->hdcp2_info.hdcp_handler.main_state) {
	case HDCP2_MS_H1P1:
		/* HDCP2_MS_H1P1 */
		/* HDCP_2_2_AUTH_FAIL */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_AUTH_FAIL) {
			pr_err("2.x: Authentication Fail!\n");
			dp_tx_hdcp2_enable_auth(hdcp_info, false);
			hdcp_info->auth_status = AUTH_FAIL;
		}
		break;

	case HDCP2_MS_A0F0:
		/* HDCP2_MS_A0F0 */
		/* HDCP_2_2_NULL_MSG */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_NULL_MSG) {
			if (!hdcp_info->hdcp2_info.enable) {
				dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_H1P1, HDCP_2_2_AUTH_FAIL);
				DPTXHDCPMSG("2.x: Sink Doesn't Support Hdcp2x!\n");
				break;
			}

			dp_tx_hdcp2_init(hdcp_info);
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1, HDCP_2_2_NULL_MSG);
			DPTXHDCPMSG("2.x: Sink Support Hdcp2x!\n");
		}
		break;

	case HDCP2_MS_A1F1:
		/* HDCP2_MS_A1F1 */
		/* HDCP_2_2_NULL_MSG */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_NULL_MSG) {
			if (hdcp_info->hdcp2_info.retry_count >= HDCP2_TX_RETRY_CNT) {
				dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_H1P1, HDCP_2_2_AUTH_FAIL);
				pr_err("2.x: Try Max Count\n");
				break;
			}

			hdcp_info->hdcp2_info.retry_count++;
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1, HDCP_2_2_AKE_INIT);
		}

		/* HDCP2_MS_A1F1 */
		/* HDCP_2_2_AKE_INIT */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_AKE_INIT) {
			tmp = dp_tx_hdcp2_write_msg(hdcp_info, HDCP_2_2_AKE_INIT);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}
			dp_tx_hdcp2_rest_variable(hdcp_info);
			hdcp_info->hdcp2_info.read_certrx = true;

			hdcp_info->hdcp2_info.hdcp_handler.send_ake_init = true;
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1, HDCP_2_2_AKE_SEND_CERT);
			pre_time = mtk_dp_get_system_time();
		}

		/* HDCP2_MS_A1F1 */
		/* HDCP_2_2_AKE_SEND_CERT */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_AKE_SEND_CERT) {
			time = mtk_dp_get_time_diff(pre_time);
			if (time < HDCP_2_2_CERT_TIMEOUT_MS) {
				msleep(20);
				break;
			}
			if (hdcp_info->hdcp2_info.read_certrx)
				dp_tx_hdcp2_read_msg(hdcp_info, HDCP_2_2_AKE_SEND_CERT);

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			ret = tee_ake_certificate(hdcp_info,
						  (u8 *)&hdcp_info->hdcp2_info.hdcp_rx.cert_rx,
						  &stored,
				hdcp_info->hdcp2_info.ake_stored_km.e_kh_km_m +
				HDCP_2_2_E_KH_KM_LEN,
				hdcp_info->hdcp2_info.ake_stored_km.e_kh_km_m);

			if (ret != RET_COMPARE_PASS) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			hdcp_info->hdcp2_info.hdcp_handler.stored_km = stored;
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1,
					      hdcp_info->hdcp2_info.hdcp_handler.stored_km ?
					HDCP_2_2_AKE_STORED_KM :
					HDCP_2_2_AKE_NO_STORED_KM);
		}

		/* HDCP2_MS_A1F1 */
		/* HDCP_2_2_AKE_NO_STORED_KM */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_AKE_NO_STORED_KM) {
			DPTXHDCPMSG("2.x: Get Km, derive Ekpub(km)\n");

			tee_enc_rsaes_oaep(hdcp_info,
					   hdcp_info->hdcp2_info.hdcp_tx.no_stored_km.e_kpub_km);
			/* Prepare e_kpub_km to send */
			tmp = dp_tx_hdcp2_write_msg(hdcp_info, HDCP_2_2_AKE_NO_STORED_KM);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1, HDCP_2_2_AKE_SEND_HPRIME);
			timeout_value = HDCP_2_2_HPRIME_NO_PAIRED_TIMEOUT_MS;
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			pre_time = mtk_dp_get_system_time();
		}

		/* HDCP2_MS_A1F1 */
		/* HDCP_2_2_AKE_STORED_KM */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_AKE_STORED_KM) {
			/* Prepare ekh_km & M to send */
			tmp = dp_tx_hdcp2_write_msg(hdcp_info, HDCP_2_2_AKE_STORED_KM);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			err_code = HDCP_ERR_NONE;
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1, HDCP_2_2_AKE_SEND_HPRIME);
			timeout_value = HDCP_2_2_HPRIME_PAIRED_TIMEOUT_MS;
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			pre_time = mtk_dp_get_system_time();
		}

		/* HDCP2_MS_A1F1 */
		/* HDCP_2_2_AKE_SEND_HPRIME */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_AKE_SEND_HPRIME) {
			if (hdcp_info->hdcp2_info.read_h_prime)
				dp_tx_hdcp2_read_msg(hdcp_info, HDCP_2_2_AKE_SEND_HPRIME);

			time = mtk_dp_get_time_diff(pre_time);
			if (time > timeout_value) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			ret = tee_ake_h_prime(hdcp_info,
					      hdcp_info->hdcp2_info.hdcp_tx.ake_init.r_tx,
					      hdcp_info->hdcp2_info.hdcp_rx.send_cert.r_rx,
				hdcp_info->hdcp2_info.hdcp_rx.send_cert.rx_caps,
				(u8 *)&hdcp_info->hdcp2_info.hdcp_tx.tx_caps,
				hdcp_info->hdcp2_info.hdcp_rx.send_hprime.h_prime,
				HDCP_2_2_H_PRIME_LEN);
			if (ret != RET_COMPARE_PASS) {
				if (hdcp_info->hdcp2_info.hdcp_handler.stored_km)
					tee_clear_paring(hdcp_info);
				err_code = HDCP_ERR_PROCESS_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (hdcp_info->hdcp2_info.hdcp_handler.stored_km)
				dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A2F2, HDCP_2_2_LC_INIT);
			else
				dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A1F1,
						      HDCP_2_2_AKE_SEND_PAIRING_INFO);

			pre_time = mtk_dp_get_system_time();
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
		}

		/* HDCP2_MS_A1F1 */
		/* HDCP_2_2_AKE_SEND_PAIRING_INFO */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state ==
			HDCP_2_2_AKE_SEND_PAIRING_INFO) {
			if (hdcp_info->hdcp2_info.read_pairing)
				dp_tx_hdcp2_read_msg(hdcp_info, HDCP_2_2_AKE_SEND_PAIRING_INFO);

			/* Ekh_Km must be available less than 200ms, Give mode time for some Rx */
			time = mtk_dp_get_time_diff(pre_time);
			if (time >	HDCP_2_2_PAIRING_TIMEOUT_MS * 2) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			/* Store m, km, Ekh(km) */
			tee_ake_paring(hdcp_info,
				       hdcp_info->hdcp2_info.hdcp_rx.pairing_info.e_kh_km);

			hdcp_info->hdcp2_info.hdcp_handler.send_pair = true;
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A2F2, HDCP_2_2_LC_INIT);
			pre_time = mtk_dp_get_system_time();
		}
		break;

	case HDCP2_MS_A2F2:
		/* HDCP2_MS_A2F2 */
		/* HDCP_2_2_LC_INIT */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_LC_INIT) {
			/* prepare Rn to send */
			tmp = dp_tx_hdcp2_write_msg(hdcp_info, HDCP_2_2_LC_INIT);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}
			hdcp_info->hdcp2_info.hdcp_handler.send_lc_init = true;

			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A2F2, HDCP_2_2_LC_SEND_LPRIME);
			pre_time = mtk_dp_get_system_time();
		}

		/* HDCP2_MS_A2F2 */
		/* HDCP_2_2_LC_SEND_LPRIME */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_LC_SEND_LPRIME) {
			time = mtk_dp_get_time_diff(pre_time);
			if (time < HDCP_2_2_DP_HPRIME_READ_TIMEOUT_MS)
				break;

			if (hdcp_info->hdcp2_info.read_l_prime)
				dp_tx_hdcp2_read_msg(hdcp_info, HDCP_2_2_LC_SEND_LPRIME);

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			ret = tee_lc_l_prime(hdcp_info, hdcp_info->hdcp2_info.hdcp_tx.lc_init.r_n,
					     hdcp_info->hdcp2_info.hdcp_rx.send_lprime.l_prime,
					     HDCP_2_2_L_PRIME_LEN);
			if (ret != RET_COMPARE_PASS) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			DPTXHDCPMSG("2.x: L' is PASS!!\n");
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A3F3, HDCP_2_2_NULL_MSG);
			pre_time = mtk_dp_get_system_time();
		}
		break;

	case HDCP2_MS_A3F3:
		/* HDCP2_MS_A3F3 */
		/* HDCP_2_2_NULL_MSG */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_NULL_MSG) {
			tee_ske_enc_ks(hdcp_info, hdcp_info->hdcp2_info.hdcp_tx.send_eks.riv,
				       hdcp_info->hdcp2_info.hdcp_tx.send_eks.e_dkey_ks);

			tmp = dp_tx_hdcp2_write_msg(hdcp_info, HDCP_2_2_SKE_SEND_EKS);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (!hdcp_info->hdcp2_info.repeater)
				dp_tx_hdcp2_write_msg(hdcp_info, HDCP_2_2_STREAM_TYPE);

			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A3F3, HDCP_2_2_SKE_SEND_EKS);
			pre_time = mtk_dp_get_system_time();
		}

		/* HDCP2_MS_A3F3 */
		/* HDCP_2_2_SKE_SEND_EKS */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_SKE_SEND_EKS) {
			time = mtk_dp_get_time_diff(pre_time);
			if (time >= HDCP_2_2_DELAY_BEFORE_ENCRYPTION_EN)
				dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A4F4, HDCP_2_2_NULL_MSG);
		}
		break;

	case HDCP2_MS_A4F4:
		/* HDCP2_MS_A4F4 */
		/* HDCP_2_2_NULL_MSG */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_NULL_MSG) {
			if (!hdcp_info->hdcp2_info.repeater) {
				dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A5F5, HDCP_2_2_AUTH_DONE);
				break;
			}
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A6F6,
					      HDCP_2_2_REP_SEND_RECVID_LIST);
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			pre_time = mtk_dp_get_system_time();
		}
		break;

	case HDCP2_MS_A5F5:
		/* HDCP2_MS_A5F5 */
		/* HDCP_2_2_AUTH_DONE */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_AUTH_DONE) {
			DPTXHDCPMSG("2.x: Authentication done!\n");
			hdcp_info->auth_status = AUTH_PASS;
			hdcp_info->hdcp2_info.retry_count = 0;
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A5F5, HDCP_2_2_NULL_MSG);
			dp_tx_hdcp2_enable_auth(hdcp_info, true);
		}
		break;

	case HDCP2_MS_A6F6:
		/* HDCP2_MS_A6F6 */
		/* HDCP_2_2_REP_SEND_RECVID_LIST */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state ==
			HDCP_2_2_REP_SEND_RECVID_LIST) {
			if (hdcp_info->hdcp2_info.read_v_prime)
				dp_tx_hdcp2_read_msg(hdcp_info, HDCP_2_2_REP_SEND_RECVID_LIST);

			time = mtk_dp_get_time_diff(pre_time);
			if (time > HDCP_2_2_RECVID_LIST_TIMEOUT_MS) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg)
				break;

			pre_time = mtk_dp_get_system_time();
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A7F7,
					      HDCP_2_2_REP_VERIFY_RECVID_LIST);
		}
		break;

	case HDCP2_MS_A7F7:
		/* HDCP2_MS_A7F7 */
		/* HDCP_2_2_REP_VERIFY_RECVID_LIST */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state ==
			HDCP_2_2_REP_VERIFY_RECVID_LIST) {
			if ((hdcp_info->hdcp2_info.hdcp_rx.receiverid_list.rx_info[1]
				& (BIT(2) | BIT(3))) != 0) {
				pr_err("2.x: DEVS_EXCEEDED or CASCADE_EXCEDDED!\n");
				err_code = HDCP_ERR_PROCESS_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			/* check seqNumV here */
			tmp = dp_tx_hdcp2_check_seq_num_v(hdcp_info);
			if (!tmp) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			tmp = dp_tx_hdcp2_recv_rep_auth_send_recv_id_list(hdcp_info);
			if (!tmp) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A8F8, HDCP_2_2_REP_SEND_ACK);
		}
		break;

	case HDCP2_MS_A8F8:
		/* HDCP2_MS_A8F8 */
		/* HDCP_2_2_REP_SEND_ACK */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_REP_SEND_ACK) {
			tmp = dp_tx_hdcp2_write_msg(hdcp_info, HDCP_2_2_REP_SEND_ACK);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			time = mtk_dp_get_time_diff(pre_time);
			if (time > HDCP2_REP_SEND_ACK) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A9F9, HDCP_2_2_REP_STREAM_MANAGE);
			hdcp_info->hdcp2_info.hdcp_handler.retry_cnt = 0;
		}
		break;

	case HDCP2_MS_A9F9:
		/* HDCP2_MS_A9F9 */
		/* HDCP_2_2_REP_STREAM_MANAGE */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_REP_STREAM_MANAGE) {
			tmp = dp_tx_hdcp2_process_rep_auth_stream_manage(hdcp_info);
			if (!tmp) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			tmp = dp_tx_hdcp2_write_msg(hdcp_info, HDCP_2_2_REP_STREAM_MANAGE);
			if (!tmp) {
				err_code = HDCP_ERR_SEND_MSG_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			pre_time = mtk_dp_get_system_time();
			hdcp_info->hdcp2_info.hdcp_handler.recv_msg = false;
			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A9F9,
					      HDCP_2_2_REP_STREAM_READY);
		}

		/* HDCP2_MS_A9F9 */
		/* HDCP_2_2_REP_STREAM_READY */
		if (hdcp_info->hdcp2_info.hdcp_handler.sub_state == HDCP_2_2_REP_STREAM_READY) {
			time = mtk_dp_get_time_diff(pre_time);
			if (time <= HDCP_2_2_STREAM_READY_TIMEOUT_MS / 2)
				break;

			dp_tx_hdcp2_read_msg(hdcp_info, HDCP_2_2_REP_STREAM_READY);

			time = mtk_dp_get_time_diff(pre_time);
			if (time > HDCP_2_2_STREAM_READY_TIMEOUT_MS) {
				err_code = HDCP_ERR_RESPONSE_TIMEROUT;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			if (!hdcp_info->hdcp2_info.hdcp_handler.recv_msg) {
				if (hdcp_info->hdcp2_info.hdcp_handler.retry_cnt
					>= HDCP2_STREAM_MANAGE_RETRY_CNT) {
					err_code = HDCP_ERR_RESPONSE_TIMEROUT;
					dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
					break;
				}

				hdcp_info->hdcp2_info.hdcp_handler.retry_cnt++;

				dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A9F9,
						      HDCP_2_2_REP_STREAM_READY);
				break;
			}

			tmp = dp_tx_hdcp2_recv_rep_auth_stream_ready(hdcp_info);
			if (!tmp) {
				err_code = HDCP_ERR_PROCESS_FAIL;
				dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
				break;
			}

			dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A5F5, HDCP_2_2_AUTH_DONE);
		}
		break;

	default:
		err_code = HDCP_ERR_UNKNOWN_STATE;
		dp_tx_hdcp2_err_handle(hdcp_info, err_code, __LINE__);
		break;
	}

	return err_code;
}

void dp_tx_hdcp2_set_start_auth(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	hdcp_info->hdcp2_info.enable = enable;

	if (enable) {
		hdcp_info->auth_status = AUTH_INIT;
		dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_A0F0, HDCP_2_2_NULL_MSG);
	} else {
		hdcp_info->auth_status = AUTH_ZERO;
		dp_tx_hdcp2_set_state(hdcp_info, HDCP2_MS_H1P1, HDCP_2_2_NULL_MSG);
		dp_tx_hdcp2_enable_auth(hdcp_info, false);
	}

	hdcp_info->hdcp2_info.retry_count = 0;
}

bool dp_tx_hdcp2_support(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 tmp[3];
	int ret;

	drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_RX_CAPS_OFFSET, tmp, HDCP_2_2_RXCAPS_LEN);

	if (HDCP_2_2_DP_HDCP_CAPABLE(tmp[2]) && tmp[0] == HDCP_2_2_RX_CAPS_VERSION_VAL) {
		hdcp_info->hdcp2_info.enable = true;
		hdcp_info->hdcp2_info.repeater = tmp[2] & BIT(0);
	} else {
		hdcp_info->hdcp2_info.enable = false;
	}

	DPTXHDCPMSG("2.x: CAPABLE: %d, Reapeater: %d\n", hdcp_info->hdcp2_info.enable,
		    hdcp_info->hdcp2_info.repeater);

	if (!hdcp_info->hdcp2_info.enable)
		return false;

	ret = tee_add_device(hdcp_info, HDCP_VERSION_2X);
	if (ret != RET_SUCCESS) {
		pr_err("2.x: HDCP TA has some error\n");
		hdcp_info->hdcp2_info.enable = false;
	}

	return hdcp_info->hdcp2_info.enable;
}

bool dp_tx_hdcp2_irq(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 rx_status = 0;

	drm_dp_dpcd_read(&mtk_dp->aux, DP_HDCP_2_2_REG_RXSTATUS_OFFSET, &rx_status,
			 HDCP_2_2_DP_RXSTATUS_LEN);

	if (rx_status & BIT(0)) {
		DPTXHDCPMSG("2.x: READY_BIT0 Ready!\n");
		hdcp_info->hdcp2_info.read_v_prime = true;
	}

	if (rx_status & BIT(1)) {
		DPTXHDCPMSG("2.x: H'_AVAILABLE Ready!\n");
		hdcp_info->hdcp2_info.read_h_prime = true;
	}

	if (rx_status & BIT(2)) {
		DPTXHDCPMSG("2.x: PAIRING_AVAILABLE Ready!\n");
		hdcp_info->hdcp2_info.read_pairing = true;
	}

	if (rx_status & BIT(4) || rx_status & BIT(3)) {
		DPTXHDCPMSG("2.x: Re-Auth HDCP2X!\n");
		dp_tx_hdcp2_set_start_auth(hdcp_info, true);
		mtk_dp_authentication(hdcp_info);
	}

	return true;
}
