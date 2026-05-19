// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cadence MHDP8501 HDMI bridge driver
 *
 * Copyright (C) 2019-2026 NXP Semiconductor, Inc.
 *
 */
#include <drm/drm_drv.h>
#include <drm/display/drm_hdmi_helper.h>
#include <drm/display/drm_hdmi_state_helper.h>
#include <drm/display/drm_scdc_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_print.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-hdmi.h>

#include "cdns-mhdp8501-core.h"

/**
 * cdns_hdmi_config_infoframe() - fill the HDMI infoframe
 * @mhdp: phandle to mhdp device.
 * @entry_id: The packet memory address in which the data is written.
 * @len: length of infoframe.
 * @buf: point to InfoFrame Packet.
 * @type: Packet Type of InfoFrame in HDMI Specification.
 *
 */

static void cdns_hdmi_clear_infoframe(struct cdns_mhdp8501_device *mhdp,
				      u8 entry_id, u8 type)
{
	u32 val;

	/* invalidate entry */
	val = F_ACTIVE_IDLE_TYPE(1) | F_PKT_ALLOC_ADDRESS(entry_id) |
	      F_PACKET_TYPE(type);
	writel(val, mhdp->regs + SOURCE_PIF_PKT_ALLOC_REG);
	writel(F_PKT_ALLOC_WR_EN(1), mhdp->regs + SOURCE_PIF_PKT_ALLOC_WR_EN);
}

static void cdns_hdmi_config_infoframe(struct cdns_mhdp8501_device *mhdp,
				       u8 entry_id, u8 len,
				       const u8 *buf, u8 type)
{
	u8 packet[32] = { 0 }, packet_len = 32;
	u32 packet32, len32;
	u32 val, i;

	/*
	 * only support 32 bytes now
	 * packet[0] = 0
	 * packet[1-3] = HB[0-2]  InfoFrame Packet Header
	 * packet[4-31] = PB[0-27] InfoFrame Packet Contents
	 */
	if (len > (packet_len - 1))
		return;

	memcpy(packet + 1, buf, len);

	cdns_hdmi_clear_infoframe(mhdp, entry_id, type);

	/* flush fifo 1 */
	writel(F_FIFO1_FLUSH(1), mhdp->regs + SOURCE_PIF_FIFO1_FLUSH);

	/* write packet into memory */
	len32 = packet_len / 4;
	for (i = 0; i < len32; i++) {
		packet32 = get_unaligned_le32(packet + 4 * i);
		writel(F_DATA_WR(packet32), mhdp->regs + SOURCE_PIF_DATA_WR);
	}

	/* write entry id */
	writel(F_WR_ADDR(entry_id), mhdp->regs + SOURCE_PIF_WR_ADDR);

	/* write request */
	writel(F_HOST_WR(1), mhdp->regs + SOURCE_PIF_WR_REQ);

	/* update entry */
	val = F_ACTIVE_IDLE_TYPE(1) | F_TYPE_VALID(1) |
	      F_PACKET_TYPE(type) | F_PKT_ALLOC_ADDRESS(entry_id);
	writel(val, mhdp->regs + SOURCE_PIF_PKT_ALLOC_REG);

	writel(F_PKT_ALLOC_WR_EN(1), mhdp->regs + SOURCE_PIF_PKT_ALLOC_WR_EN);
}

static int cdns_hdmi_get_edid_block(void *data, u8 *edid,
				    u32 block, size_t length)
{
	struct cdns_mhdp8501_device *mhdp = data;
	u8 msg[2], reg[5], i;
	int ret;

	for (i = 0; i < 4; i++) {
		msg[0] = block / 2;
		msg[1] = block % 2;

		ret = cdns_mhdp_mailbox_send_recv_multi(&mhdp->base,
							MB_MODULE_ID_HDMI_TX,
							HDMI_TX_EDID,
							sizeof(msg), msg,
							HDMI_TX_EDID,
							sizeof(reg), reg,
							length, edid);
		if (ret) {
			drm_dbg(mhdp->bridge.dev,
				"edid block read failed: %d. Retrying...\n",
				ret);
			continue;
		}

		if ((reg[3] << 8 | reg[4]) == length) {
			ret = 0;
			break;
		}

		ret = -EINVAL;
		drm_dbg(mhdp->bridge.dev,
			"edid block validation failed (len=%u/%zu). Retrying...\n",
			reg[3] << 8 | reg[4], length);
	}

	if (ret)
		dev_err(mhdp->dev, "get block[%d] edid failed: %d\n", block, ret);

	return ret;
}

static int cdns_hdmi_set_hdmi_mode_type(struct cdns_mhdp8501_device *mhdp,
					unsigned long long tmds_char_rate)
{
	u32 protocol = mhdp->hdmi.hdmi_type;
	u32 val;
	int ret;

	if (protocol == MODE_HDMI_2_0 &&
	    tmds_char_rate >= 340000000) {
		ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_CLOCK_REG_0, 0);
		if (ret)
			return ret;
		ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_CLOCK_REG_1, 0xFFFFF);
		if (ret)
			return ret;
	}

	ret = cdns_mhdp_reg_read(&mhdp->base, HDTX_CONTROLLER, &val);
	if (ret)
		return ret;

	/* set HDMI mode and preemble mode data enable */
	val |= F_HDMI_MODE(protocol) | F_HDMI2_PREAMBLE_EN(1) |
	       F_HDMI2_CTRL_IL_MODE(1);
	return cdns_mhdp_reg_write(&mhdp->base, HDTX_CONTROLLER, val);
}

static int cdns_hdmi_ctrl_init(struct cdns_mhdp8501_device *mhdp,
			       unsigned long long tmds_char_rate)
{
	u32 val;
	int ret;

	/* Set PHY to HDMI data */
	ret = cdns_mhdp_reg_write(&mhdp->base, PHY_DATA_SEL, F_SOURCE_PHY_MHDP_SEL(1));
	if (ret < 0)
		return ret;

	ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_HPD,
				  F_HPD_VALID_WIDTH(4) | F_HPD_GLITCH_WIDTH(0));
	if (ret < 0)
		return ret;

	/* open CARS */
	ret = cdns_mhdp_reg_write(&mhdp->base, SOURCE_PHY_CAR, 0xF);
	if (ret < 0)
		return ret;
	ret = cdns_mhdp_reg_write(&mhdp->base, SOURCE_HDTX_CAR, 0xFF);
	if (ret < 0)
		return ret;
	ret = cdns_mhdp_reg_write(&mhdp->base, SOURCE_PKT_CAR, 0xF);
	if (ret < 0)
		return ret;
	ret = cdns_mhdp_reg_write(&mhdp->base, SOURCE_AIF_CAR, 0xF);
	if (ret < 0)
		return ret;
	ret = cdns_mhdp_reg_write(&mhdp->base, SOURCE_CIPHER_CAR, 0xF);
	if (ret < 0)
		return ret;
	ret = cdns_mhdp_reg_write(&mhdp->base, SOURCE_CRYPTO_CAR, 0xF);
	if (ret < 0)
		return ret;
	ret = cdns_mhdp_reg_write(&mhdp->base, SOURCE_CEC_CAR, 3);
	if (ret < 0)
		return ret;

	ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_CLOCK_REG_0, 0x7c1f);
	if (ret < 0)
		return ret;
	ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_CLOCK_REG_1, 0x7c1f);
	if (ret < 0)
		return ret;

	/* init HDMI Controller */
	val = F_BCH_EN(1) | F_PIC_3D(0xF) | F_CLEAR_AVMUTE(1);
	ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_CONTROLLER, val);
	if (ret < 0)
		return ret;

	return cdns_hdmi_set_hdmi_mode_type(mhdp, tmds_char_rate);
}

static int cdns_hdmi_mode_config(struct cdns_mhdp8501_device *mhdp,
				 struct drm_display_mode *mode,
				 struct drm_connector_hdmi_state *hdmi)
{
	u32 vsync_lines = mode->vsync_end - mode->vsync_start;
	u32 eof_lines = mode->vsync_start - mode->vdisplay;
	u32 sof_lines = mode->vtotal - mode->vsync_end;
	u32 hblank = mode->htotal - mode->hdisplay;
	u32 hactive = mode->hdisplay;
	u32 vblank = mode->vtotal - mode->vdisplay;
	u32 vactive = mode->vdisplay;
	u32 hfront = mode->hsync_start - mode->hdisplay;
	u32 hback = mode->htotal - mode->hsync_end;
	u32 vfront = eof_lines;
	u32 hsync = hblank - hfront - hback;
	u32 vsync = vsync_lines;
	u32 vback = sof_lines;
	u32 v_h_polarity = ((mode->flags & DRM_MODE_FLAG_NHSYNC) ? 0 : 1) +
			   ((mode->flags & DRM_MODE_FLAG_NVSYNC) ? 0 : 2);
	int ret;
	u32 val;

	ret = cdns_mhdp_reg_write(&mhdp->base, SCHEDULER_H_SIZE, (hactive << 16) + hblank);
	if (ret < 0)
		return ret;

	ret = cdns_mhdp_reg_write(&mhdp->base, SCHEDULER_V_SIZE, (vactive << 16) + vblank);
	if (ret < 0)
		return ret;

	ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_SIGNAL_FRONT_WIDTH, (vfront << 16) + hfront);
	if (ret < 0)
		return ret;

	ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_SIGNAL_SYNC_WIDTH, (vsync << 16) + hsync);
	if (ret < 0)
		return ret;

	ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_SIGNAL_BACK_WIDTH, (vback << 16) + hback);
	if (ret < 0)
		return ret;

	ret = cdns_mhdp_reg_write(&mhdp->base, HSYNC2VSYNC_POL_CTRL, v_h_polarity);
	if (ret < 0)
		return ret;

	/* Reset Data Enable */
	ret = cdns_mhdp_reg_read(&mhdp->base, HDTX_CONTROLLER, &val);
	if (ret < 0)
		return ret;

	val &= ~F_DATA_EN(1);
	ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_CONTROLLER, val);
	if (ret < 0)
		return ret;

	/* Set bpc */
	val &= ~F_VIF_DATA_WIDTH(3);
	switch (hdmi->output_bpc) {
	case 10:
		val |= F_VIF_DATA_WIDTH(1);
		break;
	case 12:
		val |= F_VIF_DATA_WIDTH(2);
		break;
	case 16:
		val |= F_VIF_DATA_WIDTH(3);
		break;
	case 8:
	default:
		val |= F_VIF_DATA_WIDTH(0);
		break;
	}

	/* select color encoding */
	val &= ~F_HDMI_ENCODING(3);
	switch (hdmi->output_format) {
	case HDMI_COLORSPACE_YUV444:
		val |= F_HDMI_ENCODING(2);
		break;
	case HDMI_COLORSPACE_YUV422:
		val |= F_HDMI_ENCODING(1);
		break;
	case HDMI_COLORSPACE_YUV420:
		val |= F_HDMI_ENCODING(3);
		break;
	case HDMI_COLORSPACE_RGB:
	default:
		val |= F_HDMI_ENCODING(0);
		break;
	}

	ret = cdns_mhdp_reg_write(&mhdp->base, HDTX_CONTROLLER, val);
	if (ret < 0)
		return ret;

	/* set data enable */
	val |= F_DATA_EN(1);
	return cdns_mhdp_reg_write(&mhdp->base, HDTX_CONTROLLER, val);
}

static int cdns_hdmi_disable_gcp(struct cdns_mhdp8501_device *mhdp)
{
	u32 val;
	int ret;

	ret = cdns_mhdp_reg_read(&mhdp->base, HDTX_CONTROLLER, &val);
	if (ret)
		return ret;

	val &= ~F_GCP_EN(1);

	return cdns_mhdp_reg_write(&mhdp->base, HDTX_CONTROLLER, val);
}

static int cdns_hdmi_enable_gcp(struct cdns_mhdp8501_device *mhdp)
{
	u32 val;
	int ret;

	ret = cdns_mhdp_reg_read(&mhdp->base, HDTX_CONTROLLER, &val);
	if (ret)
		return ret;

	val |= F_GCP_EN(1);

	return cdns_mhdp_reg_write(&mhdp->base, HDTX_CONTROLLER, val);
}

#define HDMI_14_MAX_TMDS_CLK   (340 * 1000 * 1000)
static void cdns_hdmi_sink_config(struct cdns_mhdp8501_device *mhdp,
				  struct drm_connector *connector,
				  unsigned long long tmds_char_rate)
{
	struct drm_display_info *display = &connector->display_info;
	struct drm_scdc *scdc = &display->hdmi.scdc;
	bool hdmi_scrambling = false;
	bool hdmi_high_tmds_clock_ratio = false;

	/* check sink type (HDMI or DVI) */
	if (!display->is_hdmi) {
		mhdp->hdmi.hdmi_type = MODE_DVI;
		return;
	}

	/* Default work in HDMI1.4 */
	mhdp->hdmi.hdmi_type = MODE_HDMI_1_4;

	/* check sink support SCDC or not */
	if (!scdc->supported) {
		dev_dbg(mhdp->dev, "Sink Not Support SCDC\n");
		return;
	}

	if (tmds_char_rate > HDMI_14_MAX_TMDS_CLK) {
		hdmi_scrambling = true;
		hdmi_high_tmds_clock_ratio = true;
		mhdp->hdmi.hdmi_type = MODE_HDMI_2_0;
	} else if (scdc->scrambling.low_rates) {
		hdmi_scrambling = true;
		mhdp->hdmi.hdmi_type = MODE_HDMI_2_0;
	}

	/* Set TMDS bit clock ratio to 1/40 or 1/10, and enable/disable scrambling */
	drm_scdc_set_high_tmds_clock_ratio(connector, hdmi_high_tmds_clock_ratio);
	drm_scdc_set_scrambling(connector, hdmi_scrambling);
}

static int cdns_hdmi_bridge_attach(struct drm_bridge *bridge,
				   struct drm_encoder *encoder,
				   enum drm_bridge_attach_flags flags)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)) {
		dev_err(mhdp->dev, "do not support creating a drm_connector\n");
		return -EINVAL;
	}

	return drm_bridge_attach(encoder, bridge->next_bridge, bridge,
				 flags | DRM_BRIDGE_ATTACH_NO_CONNECTOR);
}

static int reset_pipe(struct drm_crtc *crtc)
{
	struct drm_atomic_commit *state;
	struct drm_crtc_state *crtc_state;
	struct drm_modeset_acquire_ctx ctx;
	int ret;

	state = drm_atomic_commit_alloc(crtc->dev);
	if (!state)
		return -ENOMEM;

	drm_modeset_acquire_init(&ctx, 0);

	state->acquire_ctx = &ctx;

retry:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}

	crtc_state->connectors_changed = true;

	ret = drm_atomic_commit(state);

out:
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}

	drm_atomic_commit_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}

void cdns_hdmi_handle_hotplug(struct cdns_mhdp8501_device *mhdp)
{
	struct drm_crtc *crtc = NULL;

	scoped_guard(mutex, &mhdp->link_mutex) {
		if (!mhdp->phy_powered)
			return;
		crtc = mhdp->curr_crtc;
	}

	if (!crtc)
		return;

	/*
	 * HDMI 2.0 says that one should not send scrambled data
	 * prior to configuring the sink scrambling, and that
	 * TMDS clock/data transmission should be suspended when
	 * changing the TMDS clock rate in the sink. So let's
	 * just do a full modeset here, even though some sinks
	 * would be perfectly happy if were to just reconfigure
	 * the SCDC settings on the fly.
	 */
	reset_pipe(crtc);
}

static int cdns_hdmi_i2c_write(struct cdns_mhdp8501_device *mhdp,
			       struct i2c_msg *msgs)
{
	u8 msg[5], reg[5];
	int ret;

	if (msgs->len != 2)
		return -EINVAL;

	msg[0] = msgs->addr;
	msg[1] = msgs->buf[0];
	msg[2] = 0;
	msg[3] = 1;
	msg[4] = msgs->buf[1];

	ret = cdns_mhdp_mailbox_send_recv(&mhdp->base,
					  MB_MODULE_ID_HDMI_TX, HDMI_TX_WRITE,
					  sizeof(msg), msg, sizeof(reg), reg);
	if (ret) {
		dev_err(mhdp->dev, "I2C write failed: %d\n", ret);
		return ret;
	}

	if (reg[0] != 0)
		return -EINVAL;

	return 0;
}

static int cdns_hdmi_i2c_read(struct cdns_mhdp8501_device *mhdp,
			      struct i2c_msg *msgs, int num)
{
	u8 msg[4], reg[5];
	int ret;

	/* SCDC read is a fixed 2-message transaction: [write offset][read data] */
	if (num != 2 || (msgs[0].flags & I2C_M_RD) || !(msgs[1].flags & I2C_M_RD))
		return -EINVAL;

	if (msgs[0].len < 1)
		return -EINVAL;

	msg[0] = msgs[1].addr;
	msg[1] = msgs[0].buf[0];
	put_unaligned_be16(msgs[1].len, msg + 2);

	ret = cdns_mhdp_mailbox_send_recv_multi(&mhdp->base,
						MB_MODULE_ID_HDMI_TX, HDMI_TX_READ,
						sizeof(msg), msg,
						HDMI_TX_READ,
						sizeof(reg), reg,
						msgs[1].len, msgs[1].buf);
	if (ret) {
		dev_err(mhdp->dev, "I2c Read failed: %d\n", ret);
		return ret;
	}

	if (reg[0] != 0)
		return -EINVAL;

	return 0;
}

#define  SCDC_I2C_SLAVE_ADDRESS	0x54
static int cdns_hdmi_i2c_xfer(struct i2c_adapter *adap,
			      struct i2c_msg *msgs, int num)
{
	struct cdns_mhdp8501_device *mhdp = i2c_get_adapdata(adap);
	struct cdns_hdmi_i2c *i2c = mhdp->hdmi.i2c;
	int i, ret = 0;

	/*
	 * MHDP FW provides mailbox APIs for SCDC registers access, but lacks direct I2C APIs.
	 * While individual I2C registers can be read/written using HDMI general register APIs,
	 * block reads (e.g., EDID) are not supported, making it a limited I2C interface.
	 */
	for (i = 0; i < num; i++) {
		if (msgs[i].addr != SCDC_I2C_SLAVE_ADDRESS) {
			dev_err(mhdp->dev, "ADDR=%02x is not supported\n", msgs[i].addr);
			return -EINVAL;
		}
	}

	mutex_lock(&i2c->lock);

	if (num == 1 && !(msgs[0].flags & I2C_M_RD))
		ret = cdns_hdmi_i2c_write(mhdp, msgs);
	else
		ret = cdns_hdmi_i2c_read(mhdp, msgs, num);

	if (!ret)
		ret = num;

	mutex_unlock(&i2c->lock);

	return ret;
}

static u32 cdns_hdmi_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm cdns_hdmi_algorithm = {
	.master_xfer	= cdns_hdmi_i2c_xfer,
	.functionality	= cdns_hdmi_i2c_func,
};

struct i2c_adapter *cdns_hdmi_i2c_adapter(struct cdns_mhdp8501_device *mhdp)
{
	struct i2c_adapter *adap;
	struct cdns_hdmi_i2c *i2c;
	int ret;

	i2c = devm_kzalloc(mhdp->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return ERR_PTR(-ENOMEM);

	mutex_init(&i2c->lock);

	adap = &i2c->adap;
	adap->owner = THIS_MODULE;
	adap->dev.parent = mhdp->dev;
	adap->algo = &cdns_hdmi_algorithm;
	strscpy(adap->name, "MHDP HDMI", sizeof(adap->name));
	i2c_set_adapdata(adap, mhdp);
	mhdp->hdmi.i2c = i2c;

	ret = devm_i2c_add_adapter(mhdp->dev, adap);
	if (ret) {
		dev_warn(mhdp->dev, "cannot add %s I2C adapter\n", adap->name);
		mutex_destroy(&i2c->lock);
		return ERR_PTR(ret);
	}

	return adap;
}

static enum drm_mode_status
cdns_hdmi_tmds_char_rate_valid(const struct drm_bridge *bridge,
			       const struct drm_display_mode *mode,
			       unsigned long long tmds_rate)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);
	union phy_configure_opts phy_cfg = {};
	int ret;

	phy_cfg.hdmi.tmds_char_rate = tmds_rate;

	ret = phy_validate(mhdp->phy, PHY_MODE_HDMI, 0, &phy_cfg);
	if (ret < 0)
		return MODE_CLOCK_RANGE;

	return MODE_OK;
}

static const struct drm_edid
*cdns_hdmi_bridge_edid_read(struct drm_bridge *bridge,
			    struct drm_connector *connector)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	return drm_edid_read_custom(connector, cdns_hdmi_get_edid_block, mhdp);
}

static void cdns_hdmi_bridge_atomic_disable(struct drm_bridge *bridge,
					    struct drm_atomic_commit *state)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	scoped_guard(mutex, &mhdp->link_mutex) {
		mhdp->curr_crtc = NULL;
		if (mhdp->phy_powered) {
			phy_power_off(mhdp->phy);
			mhdp->phy_powered = false;
		}
	}
}

static void cdns_hdmi_bridge_atomic_enable(struct drm_bridge *bridge,
					   struct drm_atomic_commit *state)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);
	struct drm_connector *connector;
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	struct drm_connector_hdmi_state *hdmi;
	union phy_configure_opts phy_cfg = {};
	int ret;

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

	/* Line swapping */
	ret = cdns_mhdp_reg_write(&mhdp->base, LANES_CONFIG, 0x00400000 | mhdp->lane_mapping);
	if (ret) {
		dev_err(mhdp->dev, "Failed to configure lane mapping: %d\n", ret);
		return;
	}

	hdmi = &conn_state->hdmi;

	phy_cfg.hdmi.tmds_char_rate = hdmi->tmds_char_rate;
	ret = phy_configure(mhdp->phy, &phy_cfg);
	if (ret) {
		dev_err(mhdp->dev, "%s: phy_configure() failed: %d\n",
			__func__, ret);
		return;
	}

	scoped_guard(mutex, &mhdp->link_mutex) {
		mhdp->curr_crtc = conn_state->crtc;

		phy_power_on(mhdp->phy);
		mhdp->phy_powered = true;

		cdns_hdmi_sink_config(mhdp, connector, hdmi->tmds_char_rate);

		ret = cdns_hdmi_ctrl_init(mhdp, hdmi->tmds_char_rate);
		if (ret < 0) {
			dev_err(mhdp->dev, "hdmi ctrl init failed = %d\n", ret);
			phy_power_off(mhdp->phy);
			mhdp->phy_powered = false;
			mhdp->curr_crtc = NULL;
			return;
		}

		/* Config GCP */
		if (hdmi->output_bpc == 8)
			ret = cdns_hdmi_disable_gcp(mhdp);
		else
			ret = cdns_hdmi_enable_gcp(mhdp);
		if (ret < 0) {
			dev_err(mhdp->dev, "Failed to configure GCP: %d\n", ret);
			phy_power_off(mhdp->phy);
			mhdp->phy_powered = false;
			mhdp->curr_crtc = NULL;
			return;
		}

		ret = cdns_hdmi_mode_config(mhdp, &crtc_state->adjusted_mode, hdmi);
		if (ret < 0) {
			dev_err(mhdp->dev, "CDN_API_HDMITX_SetVic_blocking ret = %d\n", ret);
			phy_power_off(mhdp->phy);
			mhdp->phy_powered = false;
			mhdp->curr_crtc = NULL;
			return;
		}

		drm_atomic_helper_connector_hdmi_update_infoframes(connector, state);
	}
}

static int cdns_hdmi_bridge_clear_avi_infoframe(struct drm_bridge *bridge)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	cdns_hdmi_clear_infoframe(mhdp, 0, HDMI_INFOFRAME_TYPE_AVI);

	return 0;
}

static int cdns_hdmi_bridge_write_avi_infoframe(struct drm_bridge *bridge,
						const u8 *buffer, size_t len)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	cdns_hdmi_config_infoframe(mhdp, 0, len, buffer, HDMI_INFOFRAME_TYPE_AVI);

	return 0;
}

static int cdns_hdmi_bridge_clear_spd_infoframe(struct drm_bridge *bridge)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	cdns_hdmi_clear_infoframe(mhdp, 1, HDMI_INFOFRAME_TYPE_SPD);

	return 0;
}

static int cdns_hdmi_bridge_write_spd_infoframe(struct drm_bridge *bridge,
						const u8 *buffer, size_t len)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	cdns_hdmi_config_infoframe(mhdp, 1, len, buffer, HDMI_INFOFRAME_TYPE_SPD);

	return 0;
}

static int cdns_hdmi_bridge_clear_hdmi_infoframe(struct drm_bridge *bridge)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	cdns_hdmi_clear_infoframe(mhdp, 2, HDMI_INFOFRAME_TYPE_VENDOR);

	return 0;
}

static int cdns_hdmi_bridge_write_hdmi_infoframe(struct drm_bridge *bridge,
						 const u8 *buffer, size_t len)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);

	cdns_hdmi_config_infoframe(mhdp, 2, len, buffer, HDMI_INFOFRAME_TYPE_VENDOR);

	return 0;
}

static int cdns_hdmi_bridge_atomic_check(struct drm_bridge *bridge,
					 struct drm_bridge_state *bridge_state,
					 struct drm_crtc_state *crtc_state,
					 struct drm_connector_state *conn_state)
{
	return drm_atomic_helper_connector_hdmi_check(conn_state->connector, conn_state->state);
}

const struct drm_bridge_funcs cdns_hdmi_bridge_funcs = {
	.attach = cdns_hdmi_bridge_attach,
	.detect = cdns_mhdp8501_detect,
	.edid_read = cdns_hdmi_bridge_edid_read,
	.mode_valid = cdns_mhdp8501_mode_valid,
	.atomic_enable = cdns_hdmi_bridge_atomic_enable,
	.atomic_disable = cdns_hdmi_bridge_atomic_disable,
	.atomic_check = cdns_hdmi_bridge_atomic_check,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.hdmi_clear_hdmi_infoframe = cdns_hdmi_bridge_clear_hdmi_infoframe,
	.hdmi_write_hdmi_infoframe = cdns_hdmi_bridge_write_hdmi_infoframe,
	.hdmi_clear_avi_infoframe = cdns_hdmi_bridge_clear_avi_infoframe,
	.hdmi_write_avi_infoframe = cdns_hdmi_bridge_write_avi_infoframe,
	.hdmi_clear_spd_infoframe = cdns_hdmi_bridge_clear_spd_infoframe,
	.hdmi_write_spd_infoframe = cdns_hdmi_bridge_write_spd_infoframe,
	.hdmi_tmds_char_rate_valid = cdns_hdmi_tmds_char_rate_valid,
};
