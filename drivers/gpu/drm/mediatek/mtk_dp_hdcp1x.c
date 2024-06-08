// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#include <drm/display/drm_hdcp_helper.h>

#include "mtk_dp_hdcp1x.h"
#include "mtk_dp_reg.h"
#include "mtk_dp.h"

#define HDCP1X_R0_WDT			100
#define HDCP1X_REP_RDY_WDT		5000

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
	if (ret & R0_AVAILABLE_DP_TRANS_P0)
		R0_available = true;
	else
		R0_available = false;

	return R0_available;
}

static void dp_tx_hdcp1x_set_repeater(struct mtk_hdcp_info *hdcp_info, bool enable)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);

	if (enable)
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_34A4,
				   REPEATER_I_DP_TRANS_P0_MASK, REPEATER_I_DP_TRANS_P0_MASK);
	else
		mtk_dp_update_bits(mtk_dp, MTK_DP_TRANS_P0_34A4, 0,  REPEATER_I_DP_TRANS_P0_MASK);
}

static int dp_tx_hdcp1x_init(struct mtk_hdcp_info *hdcp_info)
{
	int ret;
	u8 i;

	for (i = 0; i < 5; i++) {
		hdcp_info->hdcp1x_info.b_ksv[i] = 0x00;
		hdcp_info->hdcp1x_info.a_ksv[i] = 0x00;
	}

	for (i = 0; i < 5; i++)
		hdcp_info->hdcp1x_info.v[i] = 0x00;

	hdcp_info->hdcp1x_info.b_info[0] = 0x00;
	hdcp_info->hdcp1x_info.b_info[1] = 0x00;
	hdcp_info->hdcp1x_info.device_count = 0x00;

	ret = tee_hdcp_enable_encrypt(hdcp_info, false, HDCP_NONE);
	if (ret)
		return ret;

	dp_tx_hdcp1x_start_cipher(hdcp_info, false);

	ret = tee_hdcp1x_soft_rst(hdcp_info);
	if (ret)
		return ret;

	return 0;
}

static int dp_tx_hdcp1x_read_sink_b_ksv(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 read_buffer[DRM_HDCP_KSV_LEN], i;
	ssize_t ret;

	if (hdcp_info->hdcp1x_info.capable) {
		ret = drm_dp_dpcd_read(&mtk_dp->aux,
				       DP_AUX_HDCP_BKSV, read_buffer, DRM_HDCP_KSV_LEN);
		if (ret < 0)
			return ret;

		for (i = 0; i < DRM_HDCP_KSV_LEN; i++) {
			hdcp_info->hdcp1x_info.b_ksv[i] = read_buffer[i];
			dev_dbg(mtk_dp->dev, "[HDCP1.X] Bksv:0x%x\n", read_buffer[i]);
		}
	}

	return 0;
}

static int dp_tx_hdcp1x_check_sink_ksv_ready(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 read_buffer;
	ssize_t ret;

	ret = drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BSTATUS, &read_buffer, 1);
	if (ret < 0)
		return ret;

	return (read_buffer & DP_BSTATUS_READY)  ? 0 : -EAGAIN;
}

static int dp_tx_hdcp1x_read_sink_b_info(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 read_buffer[DRM_HDCP_BSTATUS_LEN];
	ssize_t ret;

	ret = drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BINFO, read_buffer, DRM_HDCP_BSTATUS_LEN);
	if (ret < 0)
		return ret;

	hdcp_info->hdcp1x_info.b_info[0] = read_buffer[0];
	hdcp_info->hdcp1x_info.b_info[1] = read_buffer[1];
	hdcp_info->hdcp1x_info.device_count = DRM_HDCP_NUM_DOWNSTREAM(read_buffer[0]);

	dev_dbg(mtk_dp->dev, "[HDCP1.X] Binfo max_cascade_EXCEEDED:%lu\n",
		DRM_HDCP_MAX_CASCADE_EXCEEDED(read_buffer[1]));
	dev_dbg(mtk_dp->dev, "[HDCP1.X] Binfo DEPTH:%d\n", read_buffer[1] & 0x07);
	dev_dbg(mtk_dp->dev, "[HDCP1.X] Binfo max_devs_EXCEEDED:%lu\n",
		DRM_HDCP_MAX_DEVICE_EXCEEDED(read_buffer[0]));
	dev_dbg(mtk_dp->dev, "[HDCP1.X] Binfo device_count:%d\n",
		hdcp_info->hdcp1x_info.device_count);

	return 0;
}

static int dp_tx_hdcp1x_read_sink_ksv(struct mtk_hdcp_info *hdcp_info, u8 dev_count)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 times = dev_count / 3;
	u8 remain = dev_count % 3;
	ssize_t ret;
	u8 i;

	if (times > 0) {
		for (i = 0; i < times; i++) {
			ret = drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_KSV_FIFO,
					       hdcp_info->hdcp1x_info.ksvfifo + i * 15, 15);
			if (ret < 0)
				return ret;
		}
	}

	if (remain > 0) {
		ret = drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_KSV_FIFO,
				       hdcp_info->hdcp1x_info.ksvfifo + times * 15, remain * 5);
		if (ret < 0)
			return ret;
	}

	dev_dbg(mtk_dp->dev, "[HDCP1.X] Read ksvfifo:0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
		hdcp_info->hdcp1x_info.ksvfifo[0],
		hdcp_info->hdcp1x_info.ksvfifo[1],
		hdcp_info->hdcp1x_info.ksvfifo[2],
		hdcp_info->hdcp1x_info.ksvfifo[3],
		hdcp_info->hdcp1x_info.ksvfifo[4]);

	return 0;
}

static int dp_tx_hdcp1x_read_sink_sha_v(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 read_buffer[4], i, j;
	ssize_t ret;

	for (i = 0; i < 5; i++) {
		ret = drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_V_PRIME(i), read_buffer, 4);
		if (ret < 0)
			return ret;

		for (j = 0; j < 4; j++) {
			hdcp_info->hdcp1x_info.v[(i * 4) + j] = read_buffer[3 - j];
			dev_dbg(mtk_dp->dev, "[HDCP1.X] Read sink V:0x%x\n",
				hdcp_info->hdcp1x_info.v[(i * 4) + j]);
		}
	}

	return 0;
}

static int dp_tx_hdcp1x_auth_with_repeater(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 *buffer;
	u32 len;
	int ret;

	if (hdcp_info->hdcp1x_info.device_count > HDCP1X_REP_MAXDEVS) {
		dev_err(mtk_dp->dev, "[HDCP1.X] Repeater:%d exceed max devs\n",
			hdcp_info->hdcp1x_info.device_count);
		return -EINVAL;
	}

	ret = dp_tx_hdcp1x_read_sink_ksv(hdcp_info, hdcp_info->hdcp1x_info.device_count);
	if (ret)
		return ret;

	ret = dp_tx_hdcp1x_read_sink_sha_v(hdcp_info);
	if (ret)
		return ret;

	len = hdcp_info->hdcp1x_info.device_count * DRM_HDCP_KSV_LEN + HDCP1X_B_INFO_LEN;
	buffer = kmalloc(len, GFP_KERNEL);
	if (!buffer) {
		dev_err(mtk_dp->dev, "[HDCP1.X] Out of Memory\n");
		return -ENOMEM;
	}

	memcpy(buffer, hdcp_info->hdcp1x_info.ksvfifo, len - HDCP1X_B_INFO_LEN);
	memcpy(buffer + (len - HDCP1X_B_INFO_LEN), hdcp_info->hdcp1x_info.b_info,
	       HDCP1X_B_INFO_LEN);
	ret = tee_hdcp1x_compute_compare_v(hdcp_info, buffer, len, hdcp_info->hdcp1x_info.v);
	if (!ret)
		dev_dbg(mtk_dp->dev, "[HDCP1.X] Check V' pass\n");
	else
		dev_err(mtk_dp->dev, "[HDCP1.X] Check V' fail\n");

	kfree(buffer);

	return ret;
}

static int dp_tx_hdcp1x_verify_b_ksv(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	int i, j, k = 0;
	u8 ksv;

	for (i = 0; i < DRM_HDCP_KSV_LEN; i++) {
		ksv = hdcp_info->hdcp1x_info.b_ksv[i];
		for (j = 0; j < 8; j++)
			k += (ksv >> j) & 0x01;
	}

	if (k != 20) {
		dev_err(mtk_dp->dev, "[HDCP1.X] Check BKSV 20'1' 20'0' fail\n");
		return -EINVAL;
	}

	return 0;
}

static int dp_tx_hdcp1x_write_a_ksv(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	int i, k, j;
	ssize_t ret;
	u8 tmp;

	ret = tee_get_aksv(hdcp_info, hdcp_info->hdcp1x_info.a_ksv);
	if (ret)
		return ret;

	ret = drm_dp_dpcd_write(&mtk_dp->aux, DP_AUX_HDCP_AKSV, hdcp_info->hdcp1x_info.a_ksv,
				DRM_HDCP_KSV_LEN);
	if (ret < 0)
		return ret;

	for (i = 0, k = 0; i < DRM_HDCP_KSV_LEN; i++) {
		tmp = hdcp_info->hdcp1x_info.a_ksv[i];

		for (j = 0; j < 8; j++)
			k += (tmp >> j) & 0x01;
		dev_dbg(mtk_dp->dev, "[HDCP1.X] Aksv:0x%x\n", tmp);
	}

	if (k != 20) {
		dev_err(mtk_dp->dev, "[HDCP1.X] Check AKSV 20'1' 20'0' fail\n");
		return -EINVAL;
	}

	return 0;
}

static int dp_tx_hdcp1x_write_an(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 an_value[DRM_HDCP_AN_LEN] = { /* on DP Spec p99 */
		0x03, 0x04, 0x07, 0x0C, 0x13, 0x1C, 0x27, 0x34};
	int ret;

	ret = tee_hdcp1x_set_tx_an(hdcp_info, an_value);
	if (ret)
		return ret;

	ret = drm_dp_dpcd_write(&mtk_dp->aux, DP_AUX_HDCP_AN, an_value, DRM_HDCP_AN_LEN);
	if (ret < 0)
		return ret;

	mdelay(5);

	return 0;
}

static int dp_tx_hdcp1x_check_r0(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 value[DRM_HDCP_BSTATUS_LEN];
	bool sink_R0_available = false;
	int i, tries;
	ssize_t ret;
	bool tmp;

	tmp = dp_tx_hdcp1x_get_r0_available(hdcp_info);
	if (!tmp) {
		dev_err(mtk_dp->dev, "[HDCP1.X] Fail to get R0 available\n");
		return -EINVAL;
	}

	tries = 2;
	for (i = 0; i < tries; i++) {
		ret = drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BSTATUS, value, 1);
		if (ret < 0)
			continue;

		sink_R0_available = (value[0x0] & DP_BSTATUS_R0_PRIME_READY) ? true : false;
		if (sink_R0_available)
			break;
	}

	if (i == tries) {
		dev_err(mtk_dp->dev, "[HDCP1.X] R0 no available\n");
		return -EINVAL;
	}

	tries = 3;
	while (i < tries) {
		ret = drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_RI_PRIME, value, DRM_HDCP_RI_LEN);
		if (ret < 0)
			return ret;

		ret = tee_compare_r0(hdcp_info, value, DRM_HDCP_RI_LEN);
		if (!ret)
			return ret;

		dev_dbg(mtk_dp->dev, "[HDCP1.X] R0 check FAIL, Rx_R0:0x%x, 0x%x, retry\n",
			value[0x1], value[0x0]);
		mdelay(5);

		i++;
	}

	dev_err(mtk_dp->dev, "[HDCP1.X] R0 check fail\n");
	return -EINVAL;
}

/* Implements Part 1 of the HDCP authorization procedure */
static int dp_tx_hdcp1x_auth(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	int ret, i, tries = 2;
	bool expired;
	ktime_t end;

	if (!hdcp_info->hdcp1x_info.capable)
		return -EAGAIN;

	ret = dp_tx_hdcp1x_init(hdcp_info);
	if (ret)
		return ret;

	ret = dp_tx_hdcp1x_write_an(hdcp_info);
	if (ret)
		return ret;
	ret = dp_tx_hdcp1x_write_a_ksv(hdcp_info);
	if (ret)
		return ret;

	for (i = 0; i < tries; i++) {
		ret = dp_tx_hdcp1x_read_sink_b_ksv(hdcp_info);
		if (ret)
			continue;

		ret = dp_tx_hdcp1x_verify_b_ksv(hdcp_info);
		if (!ret)
			break;
	}
	if (i == tries)
		return -ENODEV;
	if (drm_hdcp_check_ksvs_revoked(mtk_dp->drm_dev, hdcp_info->hdcp1x_info.b_ksv, 1) > 0) {
		dev_err(mtk_dp->dev, "[HDCP1.X] BKSV is revoked\n");
		return -EPERM;
	}

	dp_tx_hdcp1x_set_repeater(hdcp_info, hdcp_info->hdcp1x_info.repeater);

	ret = tee_calculate_lm(hdcp_info, hdcp_info->hdcp1x_info.b_ksv);
	if (ret)
		return ret;
	dp_tx_hdcp1x_start_cipher(hdcp_info, true);

	/* Wait 100ms(at least) before check R0 */
	msleep(HDCP1X_R0_WDT);
	ret = dp_tx_hdcp1x_check_r0(hdcp_info);
	if (ret)
		return ret;
	ret = tee_hdcp_enable_encrypt(hdcp_info, true, HDCP_V1);
	if (ret)
		return ret;

	if (!hdcp_info->hdcp1x_info.repeater)
		return 0;

	/* Check ksv ready (defined max time as 5s in spec) */
	end = ktime_add_ms(ktime_get_raw(), HDCP1X_REP_RDY_WDT);
	for (;;) {
		ret = dp_tx_hdcp1x_check_sink_ksv_ready(hdcp_info);
		if (!ret)
			break;

		expired = ktime_after(ktime_get_raw(), end);
		if (expired) {
			ret = -ETIMEDOUT;
			dev_err(mtk_dp->dev, "[HDCP1.X] Check sink ksv ready timeout\n");
			goto fail;
		}

		msleep(100);
	}

	ret = dp_tx_hdcp1x_check_sink_ksv_ready(hdcp_info);
	if (ret)
		goto fail;

	ret = dp_tx_hdcp1x_read_sink_b_info(hdcp_info);
	if (ret)
		goto fail;

	ret = dp_tx_hdcp1x_auth_with_repeater(hdcp_info);
	if (ret)
		goto fail;

	return 0;

fail:
	tee_hdcp_enable_encrypt(hdcp_info, false, HDCP_NONE);

	return ret;
}

void dp_tx_hdcp1x_get_info(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	u8 tmp[2];
	ssize_t ret;

	ret = drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BCAPS, tmp, 0x1);
	if (ret < 0)
		return;

	hdcp_info->hdcp1x_info.capable = tmp[0x0] & DP_BCAPS_HDCP_CAPABLE;
	hdcp_info->hdcp1x_info.repeater = tmp[0x0] & DP_BCAPS_REPEATER_PRESENT;

	dev_info(mtk_dp->dev, "[HDCP1.X] Capable:%d, Reapeater:%d\n",
		 hdcp_info->hdcp1x_info.capable,
		hdcp_info->hdcp1x_info.repeater);
}

int dp_tx_hdcp1x_enable(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	int ret = 0, i, tries = 3;

	hdcp_info->auth_status = AUTH_INIT;

	ret = tee_add_device(hdcp_info, HDCP_VERSION_1X);
	if (ret)
		goto fail;

	for (i = 0; i < tries; i++) {
		ret = dp_tx_hdcp1x_auth(hdcp_info);
		if (!ret) {
			hdcp_info->auth_version = HDCP_VERSION_1X;
			hdcp_info->auth_status = AUTH_PASS;
			dev_info(mtk_dp->dev, "[HDCP1.X] Authentication done\n");

			return 0;
		}
	}

	tee_remove_device(hdcp_info);

fail:
	hdcp_info->auth_status = AUTH_FAIL;
	dev_err(mtk_dp->dev, "[HDCP1.X] Authentication fail\n");

	return ret;
}

int dp_tx_hdcp1x_disabel(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	int ret;

	if (hdcp_info->auth_status == AUTH_PASS) {
		ret = tee_hdcp_enable_encrypt(hdcp_info, false, HDCP_NONE);
		if (ret)
			return ret;

		dp_tx_hdcp1x_start_cipher(hdcp_info, false);

		ret = tee_hdcp1x_soft_rst(hdcp_info);
		if (ret)
			return ret;
	}

	tee_remove_device(hdcp_info);

	hdcp_info->auth_version = HDCP_NONE;
	hdcp_info->auth_status = AUTH_ZERO;
	dev_info(mtk_dp->dev, "[HDCP1.X] Disable Authentication\n");

	return 0;
}

int dp_tx_hdcp1x_check_link(struct mtk_hdcp_info *hdcp_info)
{
	struct mtk_dp *mtk_dp = container_of(hdcp_info, struct mtk_dp, hdcp_info);
	int ret = -EINVAL;
	u8 bstatus;

	mutex_lock(&mtk_dp->hdcp_mutex);

	if (mtk_dp->hdcp_info.auth_status != AUTH_PASS)
		goto end;

	if (!mtk_dp->train_info.cable_plugged_in || !mtk_dp->enabled)
		goto disable;

	ret = drm_dp_dpcd_read(&mtk_dp->aux, DP_AUX_HDCP_BSTATUS, &bstatus, 1);
	if (ret != 1) {
		dev_dbg(mtk_dp->dev, "[HDCP1.X] Read bstatus failed, reauth\n");
		goto disable;
	}

	ret = bstatus & (DP_BSTATUS_LINK_FAILURE | DP_BSTATUS_REAUTH_REQ);

	if (!ret) {
		mtk_dp_hdcp_update_value(mtk_dp, DRM_MODE_CONTENT_PROTECTION_ENABLED);
		goto end;
	}

disable:
	ret = dp_tx_hdcp1x_disabel(hdcp_info);
	if (ret || !mtk_dp->train_info.cable_plugged_in || !mtk_dp->enabled) {
		ret = -EAGAIN;
		mtk_dp_hdcp_update_value(mtk_dp, DRM_MODE_CONTENT_PROTECTION_DESIRED);
		goto end;
	}

	ret = dp_tx_hdcp1x_enable(hdcp_info);
	if (ret)
		mtk_dp_hdcp_update_value(mtk_dp, DRM_MODE_CONTENT_PROTECTION_DESIRED);

end:
	mutex_unlock(&mtk_dp->hdcp_mutex);

	return ret;
}
