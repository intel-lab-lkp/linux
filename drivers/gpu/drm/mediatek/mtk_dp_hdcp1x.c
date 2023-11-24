// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019-2023 MediaTek Inc.
 */

#include "mtk_dp_hdcp1x.h"
#include "mtk_dp_reg.h"
#include "ca/tlcDpHdcp.h"
#include "mtk_dp_hdcp.h"
#include "mtk_dp.h"
#include <linux/regmap.h>

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

static void mhal_dp_tx_hdcp1x_start_cipher(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	if (enable) {
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_3480,
				       REQ_BLOCK_CIPHER_AUTH, REQ_BLOCK_CIPHER_AUTH);
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_3480,
				       KM_GENERATED, KM_GENERATED);
	} else {
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_3480, 0, KM_GENERATED);
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_3480, 0,
				       REQ_BLOCK_CIPHER_AUTH);
	}
}

static bool mhal_dp_tx_hdcp1x_get_r0_available(struct mtk_hdcp_info *hdcp_info)
{
	bool R0_available;
	u32 ret;

	ret = mtk_dp_reg_read(hdcp_info->regs, MTK_DP_TRANS_P0_34A4);
	if (ret & BIT(12))
		R0_available = true;
	else
		R0_available = false;

	return R0_available;
}

static void mhal_dp_tx_hdcp1x_set_repeater(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	if (enable)
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_34A4, BIT(15), BIT(15));
	else
		mtk_dp_reg_update_bits(hdcp_info->regs, MTK_DP_TRANS_P0_34A4, 0,  BIT(15));

#ifdef IF_ZERO
	if (hdcp_info->hdcp1x_info.repeater) {
		u8 temp;

		temp = BIT(0); /* REAUTHENTICATION_ENABLE_IRQ_HPD */
		drm_dp_dpcd_write(hdcp_info->aux, DPCD_6803B, &temp, 1);
	}
#endif
}

void mdrv_dp_tx_hdcp1x_set_start_auth(struct mtk_hdcp_info *hdcp_info, bool enable)
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
		mhal_dp_tx_hdcp1x_start_cipher(hdcp_info, false);
		tee_hdcp1x_soft_rst(hdcp_info);
	}

	hdcp_info->hdcp1x_info.retry_count = 0;
}

bool mdrv_dp_tx_hdcp1x_support(struct mtk_hdcp_info *hdcp_info)
{
	u8 temp_buffer[2];
	int ret;

	drm_dp_dpcd_read(hdcp_info->aux, DP_AUX_HDCP_BCAPS, temp_buffer, 0x1);

	hdcp_info->hdcp1x_info.enable = temp_buffer[0x0] & BIT(0);
	hdcp_info->hdcp1x_info.repeater = (temp_buffer[0x0] & BIT(1)) >> 1;

	pr_info("HDCP1.3 CAPABLE: %d, Reapeater: %d\n",
		hdcp_info->hdcp1x_info.enable,
		hdcp_info->hdcp1x_info.repeater);

	if (!hdcp_info->hdcp1x_info.enable)
		return false;

	ret = tee_add_device(hdcp_info, HDCP_VERSION_1X);
	if (ret != RET_SUCCESS) {
		pr_err("HDCP TA has some error\n");
		hdcp_info->hdcp1x_info.enable = false;
	}

	return hdcp_info->hdcp1x_info.enable;
}

static bool mdrv_dp_tx_hdcp1x_init(struct mtk_hdcp_info *hdcp_info)
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
	mhal_dp_tx_hdcp1x_start_cipher(hdcp_info, false);
	tee_hdcp1x_soft_rst(hdcp_info);

	return true;
}

static bool mdrv_dp_tx_hdcp1x_read_sink_b_ksv(struct mtk_hdcp_info *hdcp_info)
{
	u8 read_buffer[5], i;

	if (hdcp_info->hdcp1x_info.enable) {
		drm_dp_dpcd_read(hdcp_info->aux, DP_AUX_HDCP_BKSV, read_buffer, 5);

		for (i = 0; i < 5; i++) {
			hdcp_info->hdcp1x_info.b_ksv[i] =
				read_buffer[i];
			pr_info("Bksv = 0x%x\n", read_buffer[i]);
		}
	}

	return true;
}

static bool mdrv_dp_tx_hdcp1x_check_sink_ksv_ready(struct mtk_hdcp_info *hdcp_info)
{
	u8 read_buffer;

	drm_dp_dpcd_read(hdcp_info->aux, DP_AUX_HDCP_BSTATUS, &read_buffer, 1);

	hdcp_info->hdcp1x_info.ksv_ready =
		(read_buffer & BIT(0))  ? true : false;

	return hdcp_info->hdcp1x_info.ksv_ready;
}

static bool mdrv_dp_tx_hdcp1x_check_sink_cap(struct mtk_hdcp_info *hdcp_info)
{
	u8  read_buffer[0x2];

	drm_dp_dpcd_read(hdcp_info->aux, DP_AUX_HDCP_BCAPS, read_buffer, 1);

	hdcp_info->hdcp1x_info.repeater =
		(read_buffer[0] & BIT(1)) ? true : false;

	return true;
}

static bool mdrv_dp_tx_hdcp1x_read_sink_b_info(struct mtk_hdcp_info *hdcp_info)
{
	u8 read_buffer[2];

	drm_dp_dpcd_read(hdcp_info->aux, DP_AUX_HDCP_BINFO, read_buffer, 2);

	hdcp_info->hdcp1x_info.b_info[0] = read_buffer[0];
	hdcp_info->hdcp1x_info.b_info[1] = read_buffer[1];
	hdcp_info->hdcp1x_info.max_cascade =
		(read_buffer[1] & BIT(3)) ? true : false;
	hdcp_info->hdcp1x_info.max_devs =
		(read_buffer[0] & BIT(7)) ? true : false;
	hdcp_info->hdcp1x_info.device_count = read_buffer[0] & 0x7F;

	pr_info("HDCP Binfo max_cascade_EXCEEDED = %d\n",
		hdcp_info->hdcp1x_info.max_cascade);
	pr_info("HDCP Binfo DEPTH = %d\n", read_buffer[1] & 0x07);
	pr_info("HDCP Binfo max_devs_EXCEEDED = %d\n",
		hdcp_info->hdcp1x_info.max_devs);
	pr_info("HDCP Binfo device_count = %d\n",
		hdcp_info->hdcp1x_info.device_count);
	return true;
}

static bool mdrv_dp_tx_hdcp1x_read_sink_ksv(struct mtk_hdcp_info *hdcp_info,
					    u8 dev_count)
{
	u8 i;
	u8 times = dev_count / 3;
	u8 remain = dev_count % 3;

	if (times > 0) {
		for (i = 0; i < times; i++)
			drm_dp_dpcd_read(hdcp_info->aux, DP_AUX_HDCP_KSV_FIFO,
					 hdcp_info->hdcp1x_info.ksvfifo + i * 15,
				15);
	}

	if (remain > 0)
		drm_dp_dpcd_read(hdcp_info->aux, DP_AUX_HDCP_KSV_FIFO,
				 hdcp_info->hdcp1x_info.ksvfifo + times * 15,
			remain * 5);

	pr_info("HDCP Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[0]);
	pr_info("HDCP Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[1]);
	pr_info("HDCP Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[2]);
	pr_info("HDCP Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[3]);
	pr_info("HDCP Read ksvfifo = %x\n",	hdcp_info->hdcp1x_info.ksvfifo[4]);

	return true;
}

static bool mdrv_dp_tx_hdcp1x_read_sink_sha_v(struct mtk_hdcp_info *hdcp_info)
{
	u8 read_buffer[4], i, j;

	for (i = 0; i < 5; i++) {
		drm_dp_dpcd_read(hdcp_info->aux,
				 0x68014 + (i * 4), read_buffer, 4);
		for (j = 0; j < 4; j++) {
			hdcp_info->hdcp1x_info.v[(i * 4) + j] =
				read_buffer[3 - j];
			pr_info("HDCP Read sink V = %x\n",
				hdcp_info->hdcp1x_info.v[(i * 4) + j]);
		}
	}

	return true;
}

static bool mdrv_dp_tx_hdcp1x_auth_with_repeater(struct mtk_hdcp_info *hdcp_info)
{
	bool ret = false;
	u8 *buffer = NULL;
	u32 len = 0;
	int tmp = 0;

	if (hdcp_info->hdcp1x_info.device_count > HDCP1X_REP_MAXDEVS) {
		pr_err("HDCP Repeater: %d DEVs!\n",
		       hdcp_info->hdcp1x_info.device_count);
		return false;
	}

	mdrv_dp_tx_hdcp1x_read_sink_ksv(hdcp_info,
					hdcp_info->hdcp1x_info.device_count);
	mdrv_dp_tx_hdcp1x_read_sink_sha_v(hdcp_info);

	len = hdcp_info->hdcp1x_info.device_count * 5 + 2;
	buffer = kmalloc(len, GFP_KERNEL);
	if (!buffer) {
		pr_err("Out of Memory\n");
		return false;
	}

	memcpy(buffer, hdcp_info->hdcp1x_info.ksvfifo, len - 2);
	memcpy(buffer + (len - 2), hdcp_info->hdcp1x_info.b_info, 2);
	tmp = tee_hdcp1x_compute_compare_v(hdcp_info, buffer, len, hdcp_info->hdcp1x_info.v);
	if (tmp == RET_COMPARE_PASS) {
		pr_info("Check V' PASS\n");
		ret = true;
	} else {
		pr_info("Check V' Fail\n");
	}

	kfree(buffer);
	return ret;
}

static bool mdrv_dp_tx_hdcp1x_verify_b_ksv(struct mtk_hdcp_info *hdcp_info)
{
	int i, j, k = 0;
	u8 ksv;

	for (i = 0; i < 5; i++) {
		ksv = hdcp_info->hdcp1x_info.b_ksv[i];
		for (j = 0; j < 8; j++)
			k += (ksv >> j) & 0x01;
	}

	if (k != 20) {
		pr_err("Check BKSV 20'1' 20'0' Fail\n");
		return false;
	}

	return true;
}

static bool mdrv_dp_tx_hdcp1x_write_a_ksv(struct mtk_hdcp_info *hdcp_info)
{
	u8 temp;
	int i, k, j;

	tee_get_aksv(hdcp_info, hdcp_info->hdcp1x_info.a_ksv);
	drm_dp_dpcd_write(hdcp_info->aux, DP_AUX_HDCP_AKSV,
			  hdcp_info->hdcp1x_info.a_ksv, 5);

	for (i = 0, k = 0; i < 5; i++) {
		temp = hdcp_info->hdcp1x_info.a_ksv[i];

		for (j = 0; j < 8; j++)
			k += (temp >> j) & 0x01;
		pr_info("Aksv 0x%x\n", temp);
	}

	if (k != 20) {
		pr_err("Check AKSV 20'1' 20'0' Fail\n");
		return false;
	}

	return true;
}

static void mdrv_dp_tx_hdcp1x_write_an(struct mtk_hdcp_info *hdcp_info)
{
	u8 an_value[0x8] = { /* on DP Spec p99 */
		0x03, 0x04, 0x07, 0x0C, 0x13, 0x1C, 0x27, 0x34};

	tee_hdcp1x_set_tx_an(hdcp_info, an_value);
	drm_dp_dpcd_write(hdcp_info->aux, DP_AUX_HDCP_AN, an_value, 8);
	mdelay(5);
}

static bool mdrv_dp_tx_hdcp1x_check_r0(struct mtk_hdcp_info *hdcp_info)
{
	u8 temp_value[2];
	u8 retry_count = 0;
	bool sink_R0_available = false;
	bool ret;
	int tmp;

	ret = mhal_dp_tx_hdcp1x_get_r0_available(hdcp_info);
	if (!ret) {
		pr_err("HDCP ERR: R0 No Available\n");
		return false;
	}

	if (!hdcp_info->hdcp1x_info.r0_read) {
		drm_dp_dpcd_read(hdcp_info->aux, DP_AUX_HDCP_BSTATUS, temp_value, 1);
		sink_R0_available =
			((temp_value[0x0] & BIT(1)) == BIT(1)) ? true : false;

		if (!sink_R0_available) {
			drm_dp_dpcd_read(hdcp_info->aux,
					 DP_AUX_HDCP_BSTATUS, temp_value, 1);
			sink_R0_available =
				((temp_value[0x0] & BIT(1)) == BIT(1))
					? true : false;

			if (!sink_R0_available)
				return false;
		}
	}

	while (retry_count < 3) {
		drm_dp_dpcd_read(hdcp_info->aux,
				 DP_AUX_HDCP_RI_PRIME, temp_value, 2);

		tmp = tee_compare_r0(hdcp_info, temp_value, 2);
		if (tmp == RET_COMPARE_PASS)
			return true;

		pr_info("R0 check FAIL:Rx_R0=0x%x%x\n",
			temp_value[0x1], temp_value[0x0]);
		mdelay(5);

		retry_count++;
	}
	return false;
}

static void mdrv_dp_tx_hdcp1x_state_rst(struct mtk_hdcp_info *hdcp_info)
{
	pr_info("Before State Reset:(M : S)= (%d, %d)",
		hdcp_info->hdcp1x_info.main_states,
		hdcp_info->hdcp1x_info.sub_states);
	hdcp_info->hdcp1x_info.main_states = HDCP1X_main_state_A0;
	hdcp_info->hdcp1x_info.sub_states = HDCP1X_sub_FSM_IDLE;
}

void mdrv_dp_tx_hdcp1x_fsm(struct mtk_hdcp_info *hdcp_info)
{
	static int pre_main, pre_sub;
	static u32 pre_time;
	u32 time;
	bool ret;

	if (pre_main != hdcp_info->hdcp1x_info.main_states ||
	    hdcp_info->hdcp1x_info.sub_states != pre_sub) {
		pr_info("HDCP1.x State(M : S)= (%d, %d)",
			hdcp_info->hdcp1x_info.main_states,
			hdcp_info->hdcp1x_info.sub_states);
		pre_main = hdcp_info->hdcp1x_info.main_states;
		pre_sub = hdcp_info->hdcp1x_info.sub_states;
	}

	switch (hdcp_info->hdcp1x_info.main_states) {
	case HDCP1X_main_state_H2:
		switch (hdcp_info->hdcp1x_info.sub_states) {
		case HDCP1X_sub_FSM_IDLE:
			break;
		case HDCP1X_sub_FSM_auth_fail:
			tee_hdcp_enable_encrypt(hdcp_info, false, HDCP_NONE);
			pr_info("HDCP1.3 Authentication Fail\n");
			hdcp_info->auth_status = AUTH_FAIL;
			hdcp_info->hdcp1x_info.main_states =
					HDCP1X_main_state_H2;
			hdcp_info->hdcp1x_info.sub_states =
					HDCP1X_sub_FSM_IDLE;
			break;
		}
		break;
	case HDCP1X_main_state_A0:
		switch (hdcp_info->hdcp1x_info.sub_states) {
		case HDCP1X_sub_FSM_IDLE:
			if (hdcp_info->hdcp1x_info.retry_count
				> HDCP1X_REAUNTH_COUNT) {
				pr_info("Too much retry!\n");
				hdcp_info->hdcp1x_info.main_states =
					HDCP1X_main_state_H2;
				hdcp_info->hdcp1x_info.sub_states =
					HDCP1X_sub_FSM_auth_fail;
			} else {
				mdrv_dp_tx_hdcp1x_init(hdcp_info);
				hdcp_info->hdcp1x_info.main_states =
					HDCP1X_main_state_A0;
				hdcp_info->hdcp1x_info.sub_states =
					HDCP1X_sub_FSM_CHECKHDCPCAPABLE;
			}

			break;
		case HDCP1X_sub_FSM_CHECKHDCPCAPABLE:
			if (hdcp_info->hdcp1x_info.enable) {
				hdcp_info->hdcp1x_info.retry_count++;
				hdcp_info->hdcp1x_info.main_states =
					HDCP1X_main_state_A1;
				hdcp_info->hdcp1x_info.sub_states =
					HDCP1X_sub_FSM_exchange_KSV;
			} else {
				mdrv_dp_tx_hdcp1x_state_rst(hdcp_info);
			}
			break;
		}
		break;
	case HDCP1X_main_state_A1:
		switch (hdcp_info->hdcp1x_info.sub_states) {
		case HDCP1X_sub_FSM_exchange_KSV:
			mdrv_dp_tx_hdcp1x_write_an(hdcp_info);
			ret = mdrv_dp_tx_hdcp1x_write_a_ksv(hdcp_info);
			if (ret) {
				pre_time = get_system_time();
				hdcp_info->hdcp1x_info.main_states =
					HDCP1X_main_state_A1;
				hdcp_info->hdcp1x_info.sub_states =
					HDCP1X_sub_FSM_verify_bksv;
			} else {
				mdrv_dp_tx_hdcp1x_state_rst(hdcp_info);
			}
			break;

		case HDCP1X_sub_FSM_verify_bksv:
			mdrv_dp_tx_hdcp1x_read_sink_b_ksv(hdcp_info);
			mhal_dp_tx_hdcp1x_set_repeater(hdcp_info,
						       hdcp_info->hdcp1x_info.repeater);

			time = get_time_diff(pre_time);
			if (time < HDCP1X_BSTATUS_TIMEOUT_CNT) {
				pre_time = get_system_time();
				ret = mdrv_dp_tx_hdcp1x_verify_b_ksv(hdcp_info);
				if (ret) {
					hdcp_info->hdcp1x_info.main_states =
						HDCP1X_main_state_A2;
					hdcp_info->hdcp1x_info.sub_states =
						HDCP1X_sub_FSM_computation;
				} else {
					mdrv_dp_tx_hdcp1x_state_rst(hdcp_info);
					pr_info("Invalid BKSV!!\n");
				}
			} else {
				mdrv_dp_tx_hdcp1x_state_rst(hdcp_info);
				}
			break;
		}
		break;

	case HDCP1X_main_state_A2:
		switch (hdcp_info->hdcp1x_info.sub_states) {
		case HDCP1X_sub_FSM_computation:
			tee_calculate_lm(hdcp_info, hdcp_info->hdcp1x_info.b_ksv);
			mhal_dp_tx_hdcp1x_start_cipher(hdcp_info, true);
			hdcp_info->hdcp1x_info.main_states =
				HDCP1X_main_state_A3;
			hdcp_info->hdcp1x_info.sub_states =
				HDCP1X_sub_FSM_check_R0;
			pre_time = get_system_time();
			break;
		}
		break;

	case HDCP1X_main_state_A3:
		switch (hdcp_info->hdcp1x_info.sub_states) {
		case HDCP1X_sub_FSM_check_R0:
			/* Wait 100ms(at least) before check R0 */
			time = get_time_diff(pre_time);
			if (time < HDCP1X_R0_WDT &&
			    !hdcp_info->hdcp1x_info.r0_read) {
				mdelay(10);
				break;
			}

			ret = mdrv_dp_tx_hdcp1x_check_r0(hdcp_info);
			if (ret) {
				tee_hdcp_enable_encrypt(hdcp_info, true, HDCP_V1);
				hdcp_info->hdcp1x_info.main_states =
					HDCP1X_main_state_A5;
				hdcp_info->hdcp1x_info.sub_states =
					HDCP1X_sub_FSM_IDLE;
			} else {
				mdrv_dp_tx_hdcp1x_state_rst(hdcp_info);
			}

			break;
		}
		break;

	case HDCP1X_main_state_A4:
		switch (hdcp_info->hdcp1x_info.sub_states) {
		case HDCP1X_sub_FSM_IDLE:
			break;
		case HDCP1X_sub_FSM_auth_done:
			pr_info("HDCP1X: Authentication done!\n");
			hdcp_info->hdcp1x_info.retry_count = 0;
			hdcp_info->auth_status = AUTH_PASS;
			hdcp_info->hdcp1x_info.main_states =
						HDCP1X_main_state_A4;
			hdcp_info->hdcp1x_info.sub_states =
						HDCP1X_sub_FSM_IDLE;

			/* unmute */
			break;
		}
		break;

	case HDCP1X_main_state_A5:
		switch (hdcp_info->hdcp1x_info.sub_states) {
		case HDCP1X_sub_FSM_IDLE:
			mdrv_dp_tx_hdcp1x_check_sink_cap(hdcp_info);
			if (hdcp_info->hdcp1x_info.repeater) {
				pr_info("HDCP1X:Repeater!\n");
				pre_time = get_system_time();
				hdcp_info->hdcp1x_info.main_states =
					HDCP1X_main_state_A6;
				hdcp_info->hdcp1x_info.sub_states =
					HDCP1X_sub_FSM_polling_rdy_bit;
			} else {
				pr_info("HDCP1X:No Repeater!\n");
				hdcp_info->hdcp1x_info.main_states =
						HDCP1X_main_state_A4;
				hdcp_info->hdcp1x_info.sub_states =
						HDCP1X_sub_FSM_auth_done;
			}
			break;
		}
		break;

	case HDCP1X_main_state_A6:
		switch (hdcp_info->hdcp1x_info.sub_states) {
		case HDCP1X_sub_FSM_polling_rdy_bit:
			time = get_time_diff(pre_time);
			if (time > HDCP1X_REP_RDY_WDT) {
				mdrv_dp_tx_hdcp1x_state_rst(hdcp_info);
				break;
			}

			time = get_time_diff(pre_time);
			if (!hdcp_info->hdcp1x_info.ksv_ready &&
			    time > HDCP1X_REP_RDY_WDT / 2)
				mdrv_dp_tx_hdcp1x_check_sink_ksv_ready(hdcp_info);

			if (hdcp_info->hdcp1x_info.ksv_ready) {
				mdrv_dp_tx_hdcp1x_read_sink_b_info(hdcp_info);
				hdcp_info->hdcp1x_info.main_states =
					HDCP1X_main_state_A7;
				hdcp_info->hdcp1x_info.sub_states =
					HDCP1X_sub_FSM_auth_with_repeater;
				hdcp_info->hdcp1x_info.ksv_ready = false;
			}
			break;
		}
		break;

	case HDCP1X_main_state_A7:
		switch (hdcp_info->hdcp1x_info.sub_states) {
		case HDCP1X_sub_FSM_auth_with_repeater:
			if (hdcp_info->hdcp1x_info.max_cascade ||
			    hdcp_info->hdcp1x_info.max_devs){
				pr_err("MAX CASCADE or MAX DEVS!\n");
				mdrv_dp_tx_hdcp1x_state_rst(hdcp_info);
				break;
			}

			ret = mdrv_dp_tx_hdcp1x_auth_with_repeater(hdcp_info);
			if (ret) {
				hdcp_info->hdcp1x_info.main_states =
						HDCP1X_main_state_A4;
				hdcp_info->hdcp1x_info.sub_states =
						HDCP1X_sub_FSM_auth_done;
			} else {
				mdrv_dp_tx_hdcp1x_state_rst(hdcp_info);
			}

			break;
		}
		break;

	default:
		break;
	}
}
