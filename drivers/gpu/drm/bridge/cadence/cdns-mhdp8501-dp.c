// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cadence MHDP8501 DisplayPort(DP) bridge driver
 *
 * Copyright (C) 2019-2026 NXP Semiconductor, Inc.
 *
 */
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_print.h>
#include <linux/media-bus-format.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-dp.h>

#include "cdns-mhdp8501-core.h"

#define LINK_TRAINING_TIMEOUT_MS	500
#define LINK_TRAINING_RETRY_MS		20

ssize_t cdns_dp_aux_transfer(struct drm_dp_aux *aux,
			     struct drm_dp_aux_msg *msg)
{
	struct cdns_mhdp8501_device *mhdp = dev_get_drvdata(aux->dev);
	bool native = msg->request == DP_AUX_NATIVE_WRITE ||
		      msg->request == DP_AUX_NATIVE_READ;
	int ret;

	/* Ignore address only message */
	if (!msg->size || !msg->buffer) {
		msg->reply = native ?
			DP_AUX_NATIVE_REPLY_ACK : DP_AUX_I2C_REPLY_ACK;
		return msg->size;
	}

	if (!native) {
		dev_err(mhdp->dev, "%s: only native messages supported\n", __func__);
		return -EINVAL;
	}

	/* msg sanity check */
	if (msg->size > DP_AUX_MAX_PAYLOAD_BYTES) {
		dev_err(mhdp->dev, "%s: invalid msg: size(%zu), request(%x)\n",
			__func__, msg->size, (unsigned int)msg->request);
		return -EINVAL;
	}

	if (msg->request == DP_AUX_NATIVE_WRITE) {
		const u8 *buf = msg->buffer;
		int i;

		for (i = 0; i < msg->size; ++i) {
			ret = cdns_mhdp_dpcd_write(&mhdp->base,
						   msg->address + i, buf[i]);
			if (ret < 0) {
				dev_err(mhdp->dev, "Failed to write DPCD\n");
				return ret;
			}
		}
		msg->reply = DP_AUX_NATIVE_REPLY_ACK;
		return msg->size;
	}

	if (msg->request == DP_AUX_NATIVE_READ) {
		ret = cdns_mhdp_dpcd_read(&mhdp->base, msg->address,
					  msg->buffer, msg->size);
		if (ret < 0)
			return ret;
		msg->reply = DP_AUX_NATIVE_REPLY_ACK;
		return msg->size;
	}
	return 0;
}

static int cdns_dp_get_msa_misc(struct video_info *video)
{
	u32 msa_misc;
	u8 color_space = 0;
	u8 bpc = 0;

	switch (video->color_fmt) {
	/* set YUV default color space conversion to BT601 */
	case BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR444):
		color_space = 6 + BT_601 * 8;
		break;
	case BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422):
		color_space = 5 + BT_601 * 8;
		break;
	case BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR420):
		color_space = 5;
		break;
	case BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444):
	default:
		color_space = 0;
		break;
	}

	switch (video->bpc) {
	case 6:
		bpc = 0;
		break;
	case 10:
		bpc = 2;
		break;
	case 12:
		bpc = 3;
		break;
	case 16:
		bpc = 4;
		break;
	case 8:
	default:
		bpc = 1;
		break;
	}

	msa_misc = (bpc << 5) | (color_space << 1);

	return msa_misc;
}

static int cdns_dp_config_video(struct cdns_mhdp8501_device *mhdp,
				const struct drm_display_mode *mode)
{
	struct video_info *video = &mhdp->video_info;
	bool h_sync_polarity, v_sync_polarity;
	u64 symbol;
	u32 val, link_rate, rem;
	u8 bit_per_pix, tu_size_reg = TU_SIZE;
	int ret;

	bit_per_pix = (video->color_fmt == BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422)) ?
		      (video->bpc * 2) : (video->bpc * 3);

	link_rate = mhdp->dp.rate / 1000;

	ret = cdns_mhdp_reg_write(&mhdp->base, BND_HSYNC2VSYNC, VIF_BYPASS_INTERLACE);
	if (ret)
		goto err_config_video;

	ret = cdns_mhdp_reg_write(&mhdp->base, HSYNC2VSYNC_POL_CTRL, 0);
	if (ret)
		goto err_config_video;

	/*
	 * get a best tu_size and valid symbol:
	 * 1. chose Lclk freq(162Mhz, 270Mhz, 540Mhz), set TU to 32
	 * 2. calculate VS(valid symbol) = TU * Pclk * Bpp / (Lclk * Lanes)
	 * 3. if VS > *.85 or VS < *.1 or VS < 2 or TU < VS + 4, then set
	 *    TU += 2 and repeat 2nd step.
	 */
	do {
		tu_size_reg += 2;
		symbol = (u64)tu_size_reg * mode->clock * bit_per_pix;
		do_div(symbol, mhdp->dp.num_lanes * link_rate * 8);
		rem = do_div(symbol, 1000);
		if (tu_size_reg > 64) {
			ret = -EINVAL;
			dev_err(mhdp->dev, "tu error, clk:%d, lanes:%d, rate:%d\n",
				mode->clock, mhdp->dp.num_lanes, link_rate);
			goto err_config_video;
		}
	} while ((symbol <= 1) || (tu_size_reg - symbol < 4) ||
		 (rem > 850) || (rem < 100));

	val = symbol + (tu_size_reg << 8);
	val |= TU_CNT_RST_EN;
	ret = cdns_mhdp_reg_write(&mhdp->base, DP_FRAMER_TU, val);
	if (ret)
		goto err_config_video;

	/* set the FIFO Buffer size */
	val = div_u64(mode->clock * (symbol + 1), 1000) + link_rate;
	val /= (mhdp->dp.num_lanes * link_rate);
	val = div_u64(8 * (symbol + 1), bit_per_pix) - val;
	val += 2;
	ret = cdns_mhdp_reg_write(&mhdp->base, DP_VC_TABLE(15), val);
	if (ret)
		goto err_config_video;

	switch (video->bpc) {
	case 6:
		val = BCS_6;
		break;
	case 10:
		val = BCS_10;
		break;
	case 12:
		val = BCS_12;
		break;
	case 16:
		val = BCS_16;
		break;
	case 8:
	default:
		val = BCS_8;
		break;
	}

	val += video->color_fmt << 8;
	ret = cdns_mhdp_reg_write(&mhdp->base, DP_FRAMER_PXL_REPR, val);
	if (ret)
		goto err_config_video;

	v_sync_polarity = !!(mode->flags & DRM_MODE_FLAG_NVSYNC);
	h_sync_polarity = !!(mode->flags & DRM_MODE_FLAG_NHSYNC);

	val = h_sync_polarity ? DP_FRAMER_SP_HSP : 0;
	val |= v_sync_polarity ? DP_FRAMER_SP_VSP : 0;
	ret = cdns_mhdp_reg_write(&mhdp->base, DP_FRAMER_SP, val);
	if (ret)
		goto err_config_video;

	val = (mode->hsync_start - mode->hdisplay) << 16;
	val |= mode->htotal - mode->hsync_end;
	ret = cdns_mhdp_reg_write(&mhdp->base, DP_FRONT_BACK_PORCH, val);
	if (ret)
		goto err_config_video;

	val = mode->hdisplay * bit_per_pix / 8;
	ret = cdns_mhdp_reg_write(&mhdp->base, DP_BYTE_COUNT, val);
	if (ret)
		goto err_config_video;

	val = mode->htotal | ((mode->htotal - mode->hsync_start) << 16);
	ret = cdns_mhdp_reg_write(&mhdp->base, MSA_HORIZONTAL_0, val);
	if (ret)
		goto err_config_video;

	val = mode->hsync_end - mode->hsync_start;
	val |= (mode->hdisplay << 16) | (h_sync_polarity << 15);
	ret = cdns_mhdp_reg_write(&mhdp->base, MSA_HORIZONTAL_1, val);
	if (ret)
		goto err_config_video;

	val = mode->vtotal;
	val |= (mode->vtotal - mode->vsync_start) << 16;
	ret = cdns_mhdp_reg_write(&mhdp->base, MSA_VERTICAL_0, val);
	if (ret)
		goto err_config_video;

	val = mode->vsync_end - mode->vsync_start;
	val |= (mode->vdisplay << 16) | (v_sync_polarity << 15);
	ret = cdns_mhdp_reg_write(&mhdp->base, MSA_VERTICAL_1, val);
	if (ret)
		goto err_config_video;

	val = cdns_dp_get_msa_misc(video);
	ret = cdns_mhdp_reg_write(&mhdp->base, MSA_MISC, val);
	if (ret)
		goto err_config_video;

	ret = cdns_mhdp_reg_write(&mhdp->base, STREAM_CONFIG, 1);
	if (ret)
		goto err_config_video;

	val = mode->hsync_end - mode->hsync_start;
	val |= mode->hdisplay << 16;
	ret = cdns_mhdp_reg_write(&mhdp->base, DP_HORIZONTAL, val);
	if (ret)
		goto err_config_video;

	val = mode->vdisplay;
	val |= (mode->vtotal - mode->vsync_start) << 16;
	ret = cdns_mhdp_reg_write(&mhdp->base, DP_VERTICAL_0, val);
	if (ret)
		goto err_config_video;

	val = mode->vtotal;
	ret = cdns_mhdp_reg_write(&mhdp->base, DP_VERTICAL_1, val);
	if (ret)
		goto err_config_video;

	ret = cdns_mhdp_dp_reg_write_bit(&mhdp->base, DP_VB_ID, 2, 1, 0);

err_config_video:
	if (ret)
		dev_err(mhdp->dev, "config video failed: %d\n", ret);
	return ret;
}

static int cdns_dp_pixel_clk_reset(struct cdns_mhdp8501_device *mhdp)
{
	u32 val;
	int ret;

	/* reset pixel clk */
	ret = cdns_mhdp_reg_read(&mhdp->base, SOURCE_HDTX_CAR, &val);
	if (ret)
		return ret;

	ret = cdns_mhdp_reg_write(&mhdp->base, SOURCE_HDTX_CAR, val & 0xFD);
	if (ret)
		return ret;

	return cdns_mhdp_reg_write(&mhdp->base, SOURCE_HDTX_CAR, val);
}

static int cdns_dp_set_video_status(struct cdns_mhdp8501_device *mhdp, int active)
{
	u8 msg;
	int ret;

	msg = !!active;

	ret = cdns_mhdp_mailbox_send(&mhdp->base, MB_MODULE_ID_DP_TX,
				     DPTX_SET_VIDEO, sizeof(msg), &msg);
	if (ret)
		dev_err(mhdp->dev, "set video status failed: %d\n", ret);

	return ret;
}

static int cdns_dp_training_start(struct cdns_mhdp8501_device *mhdp)
{
	unsigned long timeout;
	u8 msg, event[2];
	int ret;

	msg = LINK_TRAINING_RUN;

	/* start training */
	ret = cdns_mhdp_mailbox_send(&mhdp->base, MB_MODULE_ID_DP_TX,
				     DPTX_TRAINING_CONTROL, sizeof(msg), &msg);
	if (ret)
		return ret;

	timeout = jiffies + msecs_to_jiffies(LINK_TRAINING_TIMEOUT_MS);
	while (time_before(jiffies, timeout)) {
		msleep(LINK_TRAINING_RETRY_MS);
		ret = cdns_mhdp_mailbox_send_recv(&mhdp->base, MB_MODULE_ID_DP_TX,
						  DPTX_READ_EVENT,
						  0, NULL, sizeof(event), event);
		if (ret)
			return ret;

		if (event[1] & CLK_RECOVERY_FAILED)
			dev_err(mhdp->dev, "clock recovery failed\n");
		else if (event[1] & EQ_PHASE_FINISHED)
			return 0;
	}

	return -ETIMEDOUT;
}

static int cdns_dp_get_training_status(struct cdns_mhdp8501_device *mhdp)
{
	u8 status[13];
	int ret;

	ret = cdns_mhdp_mailbox_send_recv(&mhdp->base, MB_MODULE_ID_DP_TX,
					  DPTX_READ_LINK_STAT,
					  0, NULL, sizeof(status), status);
	if (ret)
		return ret;

	mhdp->dp.rate = drm_dp_bw_code_to_link_rate(status[0]);
	mhdp->dp.num_lanes = status[1];

	if (!mhdp->dp.rate || !mhdp->dp.num_lanes) {
		dev_err(mhdp->dev, "invalid link params from FW: rate=%d lanes=%d\n",
			mhdp->dp.rate, mhdp->dp.num_lanes);
		return -EINVAL;
	}

	return ret;
}

static int cdns_dp_train_link(struct cdns_mhdp8501_device *mhdp)
{
	int ret;

	ret = cdns_dp_training_start(mhdp);
	if (ret) {
		dev_err(mhdp->dev, "Failed to start training %d\n", ret);
		return ret;
	}

	ret = cdns_dp_get_training_status(mhdp);
	if (ret) {
		dev_err(mhdp->dev, "Failed to get training stat %d\n", ret);
		return ret;
	}

	dev_dbg(mhdp->dev, "rate:0x%x, lanes:%d\n", mhdp->dp.rate,
		mhdp->dp.num_lanes);
	return ret;
}

static int cdns_dp_set_host_cap(struct cdns_mhdp8501_device *mhdp)
{
	u8 msg[8];
	int ret;

	msg[0] = drm_dp_link_rate_to_bw_code(mhdp->dp.rate);
	msg[1] = mhdp->dp.num_lanes | SCRAMBLER_EN;
	msg[2] = VOLTAGE_LEVEL_2;
	msg[3] = PRE_EMPHASIS_LEVEL_3;
	msg[4] = PTS1 | PTS2 | PTS3 | PTS4;
	msg[5] = FAST_LT_NOT_SUPPORT;
	msg[6] = mhdp->lane_mapping;
	msg[7] = ENHANCED;

	ret = cdns_mhdp_mailbox_send(&mhdp->base, MB_MODULE_ID_DP_TX,
				     DPTX_SET_HOST_CAPABILITIES,
				     sizeof(msg), msg);
	if (ret)
		dev_err(mhdp->dev, "set host cap failed: %d\n", ret);

	return ret;
}

static int cdns_dp_get_edid_block(void *data, u8 *edid,
				  unsigned int block, size_t length)
{
	struct cdns_mhdp8501_device *mhdp = data;
	u8 msg[2], reg[2], i;
	int ret;

	for (i = 0; i < 4; i++) {
		msg[0] = block / 2;
		msg[1] = block % 2;

		ret = cdns_mhdp_mailbox_send_recv_multi(&mhdp->base,
							MB_MODULE_ID_DP_TX,
							DPTX_GET_EDID,
							sizeof(msg), msg,
							DPTX_GET_EDID,
							sizeof(reg), reg,
							length, edid);
		if (ret) {
			drm_dbg_dp(mhdp->bridge.dev,
				   "edid block read failed: %d. Retrying...\n",
				   ret);
			continue;
		}

		if (reg[0] == length && reg[1] == block / 2) {
			ret = 0;
			break;
		}

		ret = -EINVAL;
		drm_dbg_dp(mhdp->bridge.dev,
			   "edid block validation failed (len=%u/%zu, blk=%u/%u). Retrying...\n",
			   reg[0], length, reg[1], block / 2);
	}

	if (ret)
		dev_err(mhdp->dev, "get block[%d] edid failed: %d\n",
			block, ret);

	return ret;
}

static int cdns_dp_mode_set(struct cdns_mhdp8501_device *mhdp,
			    const struct drm_display_mode *mode)
{
	union phy_configure_opts phy_cfg = {};
	int ret;

	ret = cdns_dp_pixel_clk_reset(mhdp);
	if (ret)
		return ret;

	/* delay for DP FW stable after pixel clock relock */
	fsleep(20000);

	/* Get DP Caps  */
	ret = drm_dp_dpcd_read(&mhdp->dp.aux, DP_DPCD_REV, mhdp->dp.dpcd,
			       DP_RECEIVER_CAP_SIZE);
	if (ret < 0) {
		dev_err(mhdp->dev, "Failed to get caps %d\n", ret);
		return ret;
	}

	mhdp->dp.rate = drm_dp_max_link_rate(mhdp->dp.dpcd);
	mhdp->dp.num_lanes = drm_dp_max_lane_count(mhdp->dp.dpcd);

	if (!mhdp->dp.rate || !mhdp->dp.num_lanes) {
		dev_err(mhdp->dev, "invalid link params from DPCD: rate=%d lanes=%d\n",
			mhdp->dp.rate, mhdp->dp.num_lanes);
		return -EINVAL;
	}

	/* check the max link rate */
	if (mhdp->dp.rate > CDNS_DP_MAX_LINK_RATE)
		mhdp->dp.rate = CDNS_DP_MAX_LINK_RATE;

	phy_cfg.dp.lanes = mhdp->dp.num_lanes;
	phy_cfg.dp.link_rate = mhdp->dp.rate;
	phy_cfg.dp.set_lanes = false;
	phy_cfg.dp.set_rate = false;
	phy_cfg.dp.set_voltages = false;

	ret = phy_configure(mhdp->phy, &phy_cfg);
	if (ret) {
		dev_err(mhdp->dev, "%s: phy_configure() failed: %d\n",
			__func__, ret);
		return ret;
	}

	/* Video off */
	ret = cdns_dp_set_video_status(mhdp, CONTROL_VIDEO_IDLE);
	if (ret) {
		dev_err(mhdp->dev, "Failed to valid video %d\n", ret);
		return ret;
	}

	/* Line swapping */
	cdns_mhdp_reg_write(&mhdp->base, LANES_CONFIG, 0x00400000 | mhdp->lane_mapping);

	/* Set DP host capability */
	ret = cdns_dp_set_host_cap(mhdp);
	if (ret) {
		dev_err(mhdp->dev, "Failed to set host cap %d\n", ret);
		return ret;
	}

	ret = cdns_mhdp_reg_write(&mhdp->base, DP_AUX_SWAP_INVERSION_CONTROL,
				  AUX_HOST_INVERT);
	if (ret) {
		dev_err(mhdp->dev, "Failed to set host invert %d\n", ret);
		return ret;
	}

	ret = cdns_dp_config_video(mhdp, mode);
	if (ret)
		dev_err(mhdp->dev, "Failed to config video %d\n", ret);

	return ret;
}

static bool
cdns_dp_needs_link_retrain(struct cdns_mhdp8501_device *mhdp)
{
	u8 link_status[DP_LINK_STATUS_SIZE];

	/*
	 * Treat an AUX read failure as needing retrain: the link may be down,
	 * which is exactly the condition that requires retraining.
	 */
	if (drm_dp_dpcd_read_phy_link_status(&mhdp->dp.aux, DP_PHY_DPRX,
					     link_status) < 0)
		return true;

	/* Retrain if link not ok */
	return !drm_dp_channel_eq_ok(link_status, mhdp->dp.num_lanes);
}

void cdns_dp_check_link_state(struct cdns_mhdp8501_device *mhdp)
{
	int ret;

	scoped_guard(mutex, &mhdp->link_mutex) {
		if (!mhdp->phy_powered)
			return;

		if (!cdns_dp_needs_link_retrain(mhdp))
			return;

		/* DP link retrain */
		ret = cdns_dp_train_link(mhdp);
		if (ret)
			dev_err(mhdp->dev, "Failed link train\n");
	}
}

static int cdns_dp_bridge_attach(struct drm_bridge *bridge,
				 struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);
	int ret;

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)) {
		dev_err(mhdp->dev, "do not support creating a drm_connector\n");
		return -EINVAL;
	}

	ret = drm_bridge_attach(encoder, bridge->next_bridge, bridge,
				flags | DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret < 0)
		return ret;

	mhdp->dp.aux.drm_dev = bridge->dev;

	return drm_dp_aux_register(&mhdp->dp.aux);
}

static void cdns_dp_bridge_detach(struct drm_bridge *bridge)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	drm_dp_aux_unregister(&mhdp->dp.aux);
}

static const struct drm_edid
*cdns_dp_bridge_edid_read(struct drm_bridge *bridge,
			  struct drm_connector *connector)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	return drm_edid_read_custom(connector, cdns_dp_get_edid_block, mhdp);
}

static u32 *cdns_dp_bridge_atomic_get_input_bus_fmts(struct drm_bridge *bridge,
						     struct drm_bridge_state *bridge_state,
						     struct drm_crtc_state *crtc_state,
						     struct drm_connector_state *conn_state,
						     u32 output_fmt,
						     unsigned int *num_input_fmts)
{
	u32 *input_fmts;

	input_fmts = kmalloc_obj(*input_fmts);
	if (!input_fmts) {
		*num_input_fmts = 0;
		return NULL;
	}

	*num_input_fmts = 1;
	input_fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;

	return input_fmts;
}

static int cdns_dp_bridge_atomic_check(struct drm_bridge *bridge,
				       struct drm_bridge_state *bridge_state,
				       struct drm_crtc_state *crtc_state,
				       struct drm_connector_state *conn_state)
{
	if (bridge_state->input_bus_cfg.format != MEDIA_BUS_FMT_RGB888_1X24)
		return -EINVAL;

	return 0;
}

static void cdns_dp_bridge_atomic_disable(struct drm_bridge *bridge,
					  struct drm_atomic_commit *state)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	scoped_guard(mutex, &mhdp->link_mutex) {
		cdns_dp_set_video_status(mhdp, CONTROL_VIDEO_IDLE);
		if (mhdp->phy_powered) {
			phy_power_off(mhdp->phy);
			mhdp->phy_powered = false;
		}
	}
}

static void cdns_dp_bridge_atomic_enable(struct drm_bridge *bridge,
					 struct drm_atomic_commit *state)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);
	struct drm_bridge_state *bridge_state;
	struct drm_connector *connector;
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	int ret;

	bridge_state = drm_atomic_get_new_bridge_state(state, bridge);
	if (WARN_ON(!bridge_state))
		return;

	connector = drm_atomic_get_new_connector_for_encoder(state,
							     bridge->encoder);
	if (WARN_ON(!connector))
		return;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (WARN_ON(!conn_state))
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (WARN_ON(!crtc_state))
		return;

	switch (bridge_state->input_bus_cfg.format) {
	case MEDIA_BUS_FMT_RGB888_1X24:
	default:
		mhdp->video_info.bpc = 8;
		mhdp->video_info.color_fmt = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444);
		break;
	}

	ret = cdns_dp_mode_set(mhdp, &crtc_state->adjusted_mode);
	if (ret)
		return;

	scoped_guard(mutex, &mhdp->link_mutex) {
		/* Power up PHY before link training */
		phy_power_on(mhdp->phy);
		mhdp->phy_powered = true;

		/* Link training */
		ret = cdns_dp_train_link(mhdp);
		if (ret) {
			dev_err(mhdp->dev, "Failed link train %d\n", ret);
			phy_power_off(mhdp->phy);
			mhdp->phy_powered = false;
			return;
		}

		ret = cdns_dp_set_video_status(mhdp, CONTROL_VIDEO_VALID);
		if (ret)
			dev_err(mhdp->dev, "Failed to valid video %d\n", ret);
	}
}

const struct drm_bridge_funcs cdns_dp_bridge_funcs = {
	.attach = cdns_dp_bridge_attach,
	.detach = cdns_dp_bridge_detach,
	.detect = cdns_mhdp8501_detect,
	.edid_read = cdns_dp_bridge_edid_read,
	.mode_valid = cdns_mhdp8501_mode_valid,
	.atomic_enable = cdns_dp_bridge_atomic_enable,
	.atomic_disable = cdns_dp_bridge_atomic_disable,
	.atomic_get_input_bus_fmts = cdns_dp_bridge_atomic_get_input_bus_fmts,
	.atomic_check = cdns_dp_bridge_atomic_check,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
};
