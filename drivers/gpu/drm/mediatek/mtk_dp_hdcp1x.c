// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#include "mtk_dp_hdcp1x.h"
#include "mtk_dp_reg.h"
#include "mtk_dp.h"

static void dp_tx_hdcp1x_start_cipher(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);

	if (enable) {
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_3480, REQ_BLOCK_CIPHER_AUTH,
				   REQ_BLOCK_CIPHER_AUTH);
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_3480, KM_GENERATED, KM_GENERATED);
	} else {
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_3480, 0, KM_GENERATED);
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_3480, 0, REQ_BLOCK_CIPHER_AUTH);
	}
}

static bool dp_tx_hdcp1x_get_r0_available(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	bool R0_available;
	u32 ret;

	ret = mtk_dp_read(mtk_dp, MTK_DP_TRANS_P0_34A4);
	if (ret & BIT(12))
		R0_available = true;
	else
		R0_available = false;

	return R0_available;
}

static void dp_tx_hdcp1x_set_repeater(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);

	if (enable)
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_34A4, BIT(15), BIT(15));
	else
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_34A4, 0,  BIT(15));
}

void dp_tx_hdcp1x_set_start_auth(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	hdcp_info->hdcp1x_info.enable = enable;

	if (enable) {
		hdcp_info->auth_status = AUTH_INIT;
		hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A0;
		hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_IDLE;
	} else {
		hdcp_info->auth_status = AUTH_ZERO;
		hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_H2;
		hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_IDLE;
		tee_hdcp_enable_encrypt(hdcp_info, false, HDCP_NONE);
		dp_tx_hdcp1x_start_cipher(hdcp_info, false);
		tee_hdcp1x_soft_rst(hdcp_info);
	}

	hdcp_info->hdcp1x_info.retry_count = 0;
}

bool dp_tx_hdcp1x_support(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 tmp[2];
	int ret;

	drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BCAPS, tmp, 0x1);

	hdcp_info->hdcp1x_info.enable = tmp[0x0] & BIT(0);
	hdcp_info->hdcp1x_info.repeater = (tmp[0x0] & BIT(1)) >> 1;

	DPTXHDCPMSG("1.x: CAPABLE: %d, Reapeater: %d\n",
		    hdcp_info->hdcp1x_info.enable,
		hdcp_info->hdcp1x_info.repeater);

	if (!hdcp_info->hdcp1x_info.enable)
		return false;

	ret = tee_add_device(hdcp_info, HDCP_VERSION_1X);
	if (ret != RET_SUCCESS) {
		DPTXHDCPERR("1.x: HDCP TA has some error\n");
		hdcp_info->hdcp1x_info.enable = false;
	}

	return hdcp_info->hdcp1x_info.enable;
}

static bool dp_tx_hdcp1x_init(struct mtk_hdcp_info *hdcp_info)
{
	u8 i;

	hdcp_info->hdcp1x_info.ksv_ready = false;
	hdcp_info->hdcp1x_info.r0_read = false;
	hdcp_info->hdcp1x_info.b_status = 0x00;
	for (i = 0; i < 5; i++) {
		hdcp_info->hdcp1x_info.b_ksv[i] = 0x00;
		hdcp_info->hdcp1x_info.a_ksv[i] = 0x00;
	}

	for (i = 0; i < 5; i++)
		hdcp_info->hdcp1x_info.v[i] = 0x00;

	hdcp_info->hdcp1x_info.b_info[0] = 0x00;
	hdcp_info->hdcp1x_info.b_info[1] = 0x00;
	hdcp_info->hdcp1x_info.max_cascade = false;
	hdcp_info->hdcp1x_info.max_devs = false;
	hdcp_info->hdcp1x_info.device_count = 0x00;

	tee_hdcp_enable_encrypt(hdcp_info, false, HDCP_NONE);
	dp_tx_hdcp1x_start_cipher(hdcp_info, false);
	tee_hdcp1x_soft_rst(hdcp_info);

	return true;
}

static bool dp_tx_hdcp1x_read_sink_b_ksv(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 read_buffer[DRM_HDCP_KSV_LEN], i;

	if (hdcp_info->hdcp1x_info.enable) {
		drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BKSV, read_buffer, DRM_HDCP_KSV_LEN);

		for (i = 0; i < DRM_HDCP_KSV_LEN; i++) {
			hdcp_info->hdcp1x_info.b_ksv[i] = read_buffer[i];
			DPTXHDCPMSG("1.x: Bksv = 0x%x\n", read_buffer[i]);
		}
	}

	return true;
}

static bool dp_tx_hdcp1x_check_sink_ksv_ready(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 read_buffer;

	drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BSTATUS, &read_buffer, 1);

	hdcp_info->hdcp1x_info.ksv_ready = (read_buffer & BIT(0))  ? true : false;

	return hdcp_info->hdcp1x_info.ksv_ready;
}

static bool dp_tx_hdcp1x_check_sink_cap(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8  read_buffer[0x2];

	drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BCAPS, read_buffer, 1);

	hdcp_info->hdcp1x_info.repeater = (read_buffer[0] & BIT(1)) ? true : false;

	return true;
}

static bool dp_tx_hdcp1x_read_sink_b_info(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 read_buffer[DRM_HDCP_BSTATUS_LEN];

	drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BINFO, read_buffer, DRM_HDCP_BSTATUS_LEN);

	hdcp_info->hdcp1x_info.b_info[0] = read_buffer[0];
	hdcp_info->hdcp1x_info.b_info[1] = read_buffer[1];
	hdcp_info->hdcp1x_info.max_cascade = (read_buffer[1] & BIT(3)) ? true : false;
	hdcp_info->hdcp1x_info.max_devs = (read_buffer[0] & BIT(7)) ? true : false;
	hdcp_info->hdcp1x_info.device_count = read_buffer[0] & 0x7F;

	DPTXHDCPMSG("1.x: Binfo max_cascade_EXCEEDED = %d\n", hdcp_info->hdcp1x_info.max_cascade);
	DPTXHDCPMSG("1.x: Binfo DEPTH = %d\n", read_buffer[1] & 0x07);
	DPTXHDCPMSG("1.x: Binfo max_devs_EXCEEDED = %d\n", hdcp_info->hdcp1x_info.max_devs);
	DPTXHDCPMSG("1.x: Binfo device_count = %d\n", hdcp_info->hdcp1x_info.device_count);
	return true;
}

static bool dp_tx_hdcp1x_read_sink_ksv(struct mtk_hdcp_info *hdcp_info, u8 dev_count)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 i;
	u8 times = dev_count / 3;
	u8 remain = dev_count % 3;

	if (times > 0) {
		for (i = 0; i < times; i++)
			drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_KSV_FIFO,
					 hdcp_info->hdcp1x_info.ksvfifo + i * 15, 15);
	}

	if (remain > 0)
		drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_KSV_FIFO,
				 hdcp_info->hdcp1x_info.ksvfifo + times * 15, remain * 5);

	DPTXHDCPMSG("1.x: Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[0]);
	DPTXHDCPMSG("1.x: Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[1]);
	DPTXHDCPMSG("1.x: Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[2]);
	DPTXHDCPMSG("1.x: Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[3]);
	DPTXHDCPMSG("1.x: Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[4]);

	return true;
}

static bool dp_tx_hdcp1x_read_sink_sha_v(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 read_buffer[4], i, j;

	for (i = 0; i < 5; i++) {
		drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_V_PRIME(i), read_buffer, 4);
		for (j = 0; j < 4; j++) {
			hdcp_info->hdcp1x_info.v[(i * 4) + j] = read_buffer[3 - j];
			DPTXHDCPMSG("1.x: Read sink V = %x\n",
				    hdcp_info->hdcp1x_info.v[(i * 4) + j]);
		}
	}

	return true;
}

static bool dp_tx_hdcp1x_auth_with_repeater(struct mtk_hdcp_info *hdcp_info)
{
	bool ret = false;
	u8 *buffer = NULL;
	u32 len = 0;
	int tmp = 0;

	if (hdcp_info->hdcp1x_info.device_count > HDCP1X_REP_MAXDEVS) {
		DPTXHDCPERR("1.x: Repeater: %d DEVs!\n", hdcp_info->hdcp1x_info.device_count);
		return false;
	}

	dp_tx_hdcp1x_read_sink_ksv(hdcp_info, hdcp_info->hdcp1x_info.device_count);
	dp_tx_hdcp1x_read_sink_sha_v(hdcp_info);

	len = hdcp_info->hdcp1x_info.device_count * DRM_HDCP_KSV_LEN + HDCP1X_B_INFO_LEN;
	buffer = kmalloc(len, GFP_KERNEL);
	if (!buffer) {
		DPTXHDCPERR("1.x: Out of Memory\n");
		return false;
	}

	memcpy(buffer, hdcp_info->hdcp1x_info.ksvfifo, len - HDCP1X_B_INFO_LEN);
	memcpy(buffer + (len - HDCP1X_B_INFO_LEN), hdcp_info->hdcp1x_info.b_info,
	       HDCP1X_B_INFO_LEN);
	tmp = tee_hdcp1x_compute_compare_v(hdcp_info, buffer, len, hdcp_info->hdcp1x_info.v);
	if (tmp == RET_COMPARE_PASS) {
		DPTXHDCPMSG("1.x: Check V' PASS\n");
		ret = true;
	} else {
		DPTXHDCPMSG("1.x: Check V' Fail\n");
	}

	kfree(buffer);
	return ret;
}

static bool dp_tx_hdcp1x_verify_b_ksv(struct mtk_hdcp_info *hdcp_info)
{
	int i, j, k = 0;
	u8 ksv;

	for (i = 0; i < DRM_HDCP_KSV_LEN; i++) {
		ksv = hdcp_info->hdcp1x_info.b_ksv[i];
		for (j = 0; j < 8; j++)
			k += (ksv >> j) & 0x01;
	}

	if (k != 20) {
		DPTXHDCPERR("1.x: Check BKSV 20'1' 20'0' Fail\n");
		return false;
	}

	return true;
}

static bool dp_tx_hdcp1x_write_a_ksv(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 tmp;
	int i, k, j;

	tee_get_aksv(hdcp_info, hdcp_info->hdcp1x_info.a_ksv);
	drm_dp_dpcd_write(&mtk_dp->aux, DP_AUX_HDCP_AKSV, hdcp_info->hdcp1x_info.a_ksv,
			  DRM_HDCP_KSV_LEN);

	for (i = 0, k = 0; i < DRM_HDCP_KSV_LEN; i++) {
		tmp = hdcp_info->hdcp1x_info.a_ksv[i];

		for (j = 0; j < 8; j++)
			k += (tmp >> j) & 0x01;
		DPTXHDCPMSG("1.x: Aksv 0x%x\n", tmp);
	}

	if (k != 20) {
		DPTXHDCPERR("1.x: Check AKSV 20'1' 20'0' Fail\n");
		return false;
	}

	return true;
}

static void dp_tx_hdcp1x_write_an(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 an_value[DRM_HDCP_AN_LEN] = { /* on DP Spec p99 */
		0x03, 0x04, 0x07, 0x0C, 0x13, 0x1C, 0x27, 0x34};

	tee_hdcp1x_set_tx_an(hdcp_info, an_value);
	drm_dp_dpcd_write(&mtk_dp->aux, DP_AUX_HDCP_AN, an_value, DRM_HDCP_AN_LEN);
	mdelay(5);
}

static bool dp_tx_hdcp1x_check_r0(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 value[DRM_HDCP_BSTATUS_LEN];
	u8 retry_count = 0;
	bool sink_R0_available = false;
	bool ret;
	int tmp;

	ret = dp_tx_hdcp1x_get_r0_available(hdcp_info);
	if (!ret) {
		DPTXHDCPERR("1.x: ERR: R0 No Available\n");
		return false;
	}

	if (!hdcp_info->hdcp1x_info.r0_read) {
		drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BSTATUS, value, 1);
		sink_R0_available = ((value[0x0] & BIT(1)) == BIT(1)) ? true : false;

		if (!sink_R0_available) {
			drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BSTATUS, value, 1);
			sink_R0_available = ((value[0x0] & BIT(1)) == BIT(1)) ? true : false;

			if (!sink_R0_available)
				return false;
		}
	}

	while (retry_count < 3) {
		drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_RI_PRIME, value, DRM_HDCP_RI_LEN);

		tmp = tee_compare_r0(hdcp_info, value, DRM_HDCP_RI_LEN);
		if (tmp == RET_COMPARE_PASS)
			return true;

		DPTXHDCPMSG("1.x: R0 check FAIL:Rx_R0=0x%x%x\n", value[0x1], value[0x0]);
		mdelay(5);

		retry_count++;
	}
	return false;
}

static void dp_tx_hdcp1x_state_rst(struct mtk_hdcp_info *hdcp_info)
{
	DPTXHDCPMSG("1.x: Before State Reset:(M : S)= (%d, %d)",
		    hdcp_info->hdcp1x_info.main_states,
		hdcp_info->hdcp1x_info.sub_states);
	hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A0;
	hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_IDLE;
}

void dp_tx_hdcp1x_fsm(struct mtk_hdcp_info *hdcp_info)
{
	static int pre_main, pre_sub;
	static u32 pre_time;
	u32 time;
	bool ret;

	if (pre_main != hdcp_info->hdcp1x_info.main_states ||
	    hdcp_info->hdcp1x_info.sub_states != pre_sub) {
		DPTXHDCPMSG("1.x: State(M : S)= (%d, %d)",
			    hdcp_info->hdcp1x_info.main_states,
			hdcp_info->hdcp1x_info.sub_states);
		pre_main = hdcp_info->hdcp1x_info.main_states;
		pre_sub = hdcp_info->hdcp1x_info.sub_states;
	}

	switch (hdcp_info->hdcp1x_info.main_states) {
	case HDCP1X_main_state_H2:
		/* HDCP1X_main_state_H2 */
		/* HDCP1X_sub_FSM_auth_fail */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_auth_fail) {
			tee_hdcp_enable_encrypt(hdcp_info, false, HDCP_NONE);
			DPTXHDCPMSG("1.x: Authentication Fail\n");
			hdcp_info->auth_status = AUTH_FAIL;
			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_H2;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_IDLE;
		}
		break;

	case HDCP1X_main_state_A0:
		/* HDCP1X_main_state_A0 */
		/* HDCP1X_sub_FSM_IDLE */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_IDLE) {
			if (hdcp_info->hdcp1x_info.retry_count > HDCP1X_REAUNTH_COUNT) {
				DPTXHDCPMSG("1.x: Too much retry!\n");
				hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_H2;
				hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_auth_fail;
				break;
			}

			dp_tx_hdcp1x_init(hdcp_info);
			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A0;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_CHECKHDCPCAPABLE;
		}

		/* HDCP1X_main_state_A0 */
		/* HDCP1X_sub_FSM_CHECKHDCPCAPABLE */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_CHECKHDCPCAPABLE) {
			if (!hdcp_info->hdcp1x_info.enable) {
				dp_tx_hdcp1x_state_rst(hdcp_info);
				break;
			}

			hdcp_info->hdcp1x_info.retry_count++;
			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A1;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_exchange_KSV;
		}
		break;

	case HDCP1X_main_state_A1:
		/* HDCP1X_main_state_A1 */
		/* HDCP1X_sub_FSM_exchange_KSV */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_exchange_KSV) {
			dp_tx_hdcp1x_write_an(hdcp_info);
			ret = dp_tx_hdcp1x_write_a_ksv(hdcp_info);
			if (!ret) {
				dp_tx_hdcp1x_state_rst(hdcp_info);
				break;
			}

			pre_time = mtk_dp_get_system_time();
			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A1;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_verify_bksv;
		}

		/* HDCP1X_main_state_A1 */
		/* HDCP1X_sub_FSM_verify_bksv */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_verify_bksv) {
			dp_tx_hdcp1x_read_sink_b_ksv(hdcp_info);
			dp_tx_hdcp1x_set_repeater(hdcp_info, hdcp_info->hdcp1x_info.repeater);

			time = mtk_dp_get_time_diff(pre_time);
			if (time >= HDCP1X_BSTATUS_TIMEOUT_CNT) {
				dp_tx_hdcp1x_state_rst(hdcp_info);
				break;
			}

			pre_time = mtk_dp_get_system_time();
			ret = dp_tx_hdcp1x_verify_b_ksv(hdcp_info);
			if (!ret) {
				dp_tx_hdcp1x_state_rst(hdcp_info);
				DPTXHDCPMSG("1.x: Invalid BKSV!!\n");
				break;
			}

			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A2;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_computation;
		}
		break;

	case HDCP1X_main_state_A2:
		/* HDCP1X_main_state_A2 */
		/* HDCP1X_sub_FSM_computation */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_computation) {
			tee_calculate_lm(hdcp_info, hdcp_info->hdcp1x_info.b_ksv);
			dp_tx_hdcp1x_start_cipher(hdcp_info, true);
			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A3;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_check_R0;
			pre_time = mtk_dp_get_system_time();
		}
		break;

	case HDCP1X_main_state_A3:
		/* HDCP1X_main_state_A3 */
		/* HDCP1X_sub_FSM_check_R0 */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_check_R0) {
			/* Wait 100ms(at least) before check R0 */
			time = mtk_dp_get_time_diff(pre_time);
			if (time < HDCP1X_R0_WDT && !hdcp_info->hdcp1x_info.r0_read) {
				mdelay(10);
				break;
			}

			ret = dp_tx_hdcp1x_check_r0(hdcp_info);
			if (!ret) {
				dp_tx_hdcp1x_state_rst(hdcp_info);
				break;
			}

			tee_hdcp_enable_encrypt(hdcp_info, true, HDCP_V1);
			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A5;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_IDLE;
		}
		break;

	case HDCP1X_main_state_A4:
		/* HDCP1X_main_state_A4 */
		/* HDCP1X_sub_FSM_auth_done */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_auth_done) {
			DPTXHDCPMSG("1.x: Authentication done!\n");
			hdcp_info->hdcp1x_info.retry_count = 0;
			hdcp_info->auth_status = AUTH_PASS;
			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A4;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_IDLE;

			/* unmute */
		}
		break;

	case HDCP1X_main_state_A5:
		/* HDCP1X_main_state_A5 */
		/* HDCP1X_sub_FSM_IDLE */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_IDLE) {
			dp_tx_hdcp1x_check_sink_cap(hdcp_info);
			if (!hdcp_info->hdcp1x_info.repeater) {
				DPTXHDCPMSG("1.x: No Repeater!\n");
				hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A4;
				hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_auth_done;
				break;
			}

			DPTXHDCPMSG("1.x: Repeater!\n");
			pre_time = mtk_dp_get_system_time();
			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A6;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_polling_rdy_bit;
		}
		break;

	case HDCP1X_main_state_A6:
		/* HDCP1X_main_state_A6 */
		/* HDCP1X_sub_FSM_polling_rdy_bit */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_polling_rdy_bit) {
			time = mtk_dp_get_time_diff(pre_time);
			if (time > HDCP1X_REP_RDY_WDT) {
				dp_tx_hdcp1x_state_rst(hdcp_info);
				break;
			}

			time = mtk_dp_get_time_diff(pre_time);
			if (!hdcp_info->hdcp1x_info.ksv_ready && time > HDCP1X_REP_RDY_WDT / 2)
				dp_tx_hdcp1x_check_sink_ksv_ready(hdcp_info);

			if (hdcp_info->hdcp1x_info.ksv_ready) {
				dp_tx_hdcp1x_read_sink_b_info(hdcp_info);
				hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A7;
				hdcp_info->hdcp1x_info.sub_states =
					HDCP1X_sub_FSM_auth_with_repeater;
				hdcp_info->hdcp1x_info.ksv_ready = false;
			}
		}
		break;

	case HDCP1X_main_state_A7:
		/* HDCP1X_main_state_A7 */
		/* HDCP1X_sub_FSM_auth_with_repeater */
		if (hdcp_info->hdcp1x_info.sub_states == HDCP1X_sub_FSM_auth_with_repeater) {
			if (hdcp_info->hdcp1x_info.max_cascade || hdcp_info->hdcp1x_info.max_devs) {
				DPTXHDCPERR("1.x: MAX CASCADE or MAX DEVS!\n");
				dp_tx_hdcp1x_state_rst(hdcp_info);
				break;
			}

			ret = dp_tx_hdcp1x_auth_with_repeater(hdcp_info);
			if (!ret) {
				dp_tx_hdcp1x_state_rst(hdcp_info);
				break;
			}

			hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A4;
			hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_auth_done;
		}
		break;

	default:
		break;
	}
}
