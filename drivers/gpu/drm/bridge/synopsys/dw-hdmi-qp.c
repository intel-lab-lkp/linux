// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2022 Rockchip Electronics Co., Ltd.
 * Copyright (c) 2024 Collabora Ltd.
 *
 * Author: Algea Cao <algea.cao@rock-chips.com>
 * Author: Cristian Ciocaltea <cristian.ciocaltea@collabora.com>
 */
#include <linux/hdmi.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>

#include <drm/display/drm_hdmi_helper.h>
#include <drm/display/drm_scdc_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>

#include "dw-hdmi-common.h"
#include "dw-hdmi-qp.h"

static void dw_hdmi_qp_write(struct dw_hdmi *hdmi, unsigned int val, int offset)
{
	regmap_write(hdmi->regm, offset, val);
}

static unsigned int dw_hdmi_qp_read(struct dw_hdmi *hdmi, int offset)
{
	unsigned int val = 0;

	regmap_read(hdmi->regm, offset, &val);

	return val;
}

static void dw_hdmi_qp_mod(struct dw_hdmi *hdmi, unsigned int data,
			   unsigned int mask, unsigned int reg)
{
	regmap_update_bits(hdmi->regm, reg, mask, data);
}

static void dw_hdmi_qp_i2c_init(struct dw_hdmi *hdmi)
{
	/* Software reset */
	dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);

	dw_hdmi_qp_write(hdmi, 0x085c085c, I2CM_FM_SCL_CONFIG0);

	dw_hdmi_qp_mod(hdmi, 0, I2CM_FM_EN, I2CM_INTERFACE_CONTROL0);

	/* Clear DONE and ERROR interrupts */
	dw_hdmi_qp_write(hdmi, I2CM_OP_DONE_CLEAR | I2CM_NACK_RCVD_CLEAR,
			 MAINUNIT_1_INT_CLEAR);
}

static int dw_hdmi_qp_i2c_read(struct dw_hdmi *hdmi,
			       unsigned char *buf, unsigned int length)
{
	struct dw_hdmi_i2c *i2c = hdmi->i2c;
	int stat;

	if (!i2c->is_regaddr) {
		dev_dbg(hdmi->dev, "set read register address to 0\n");
		i2c->slave_reg = 0x00;
		i2c->is_regaddr = true;
	}

	while (length--) {
		reinit_completion(&i2c->cmp);

		dw_hdmi_qp_mod(hdmi, i2c->slave_reg++ << 12, I2CM_ADDR,
			       I2CM_INTERFACE_CONTROL0);

		dw_hdmi_qp_mod(hdmi, I2CM_FM_READ, I2CM_WR_MASK,
			       I2CM_INTERFACE_CONTROL0);

		stat = wait_for_completion_timeout(&i2c->cmp, HZ / 10);
		if (!stat) {
			dev_err(hdmi->dev, "i2c read timed out\n");
			dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);
			return -EAGAIN;
		}

		/* Check for error condition on the bus */
		if (i2c->stat & I2CM_NACK_RCVD_IRQ) {
			dev_err(hdmi->dev, "i2c read error\n");
			dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);
			return -EIO;
		}

		*buf++ = dw_hdmi_qp_read(hdmi, I2CM_INTERFACE_RDDATA_0_3) & 0xff;
		dw_hdmi_qp_mod(hdmi, 0, I2CM_WR_MASK, I2CM_INTERFACE_CONTROL0);
	}

	i2c->is_segment = false;

	return 0;
}

static int dw_hdmi_qp_i2c_write(struct dw_hdmi *hdmi,
				unsigned char *buf, unsigned int length)
{
	struct dw_hdmi_i2c *i2c = hdmi->i2c;
	int stat;

	if (!i2c->is_regaddr) {
		/* Use the first write byte as register address */
		i2c->slave_reg = buf[0];
		length--;
		buf++;
		i2c->is_regaddr = true;
	}

	while (length--) {
		reinit_completion(&i2c->cmp);

		dw_hdmi_qp_write(hdmi, *buf++, I2CM_INTERFACE_WRDATA_0_3);
		dw_hdmi_qp_mod(hdmi, i2c->slave_reg++ << 12, I2CM_ADDR,
			       I2CM_INTERFACE_CONTROL0);
		dw_hdmi_qp_mod(hdmi, I2CM_FM_WRITE, I2CM_WR_MASK,
			       I2CM_INTERFACE_CONTROL0);

		stat = wait_for_completion_timeout(&i2c->cmp, HZ / 10);
		if (!stat) {
			dev_err(hdmi->dev, "i2c write time out!\n");
			dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);
			return -EAGAIN;
		}

		/* Check for error condition on the bus */
		if (i2c->stat & I2CM_NACK_RCVD_IRQ) {
			dev_err(hdmi->dev, "i2c write nack!\n");
			dw_hdmi_qp_write(hdmi, 0x01, I2CM_CONTROL0);
			return -EIO;
		}

		dw_hdmi_qp_mod(hdmi, 0, I2CM_WR_MASK, I2CM_INTERFACE_CONTROL0);
	}

	return 0;
}

static int dw_hdmi_qp_i2c_xfer(struct i2c_adapter *adap,
			       struct i2c_msg *msgs, int num)
{
	struct dw_hdmi *hdmi = i2c_get_adapdata(adap);
	struct dw_hdmi_i2c *i2c = hdmi->i2c;
	u8 addr = msgs[0].addr;
	int i, ret = 0;

	if (addr == DDC_CI_ADDR)
		/*
		 * The internal I2C controller does not support the multi-byte
		 * read and write operations needed for DDC/CI.
		 * TOFIX: Blacklist the DDC/CI address until we filter out
		 * unsupported I2C operations.
		 */
		return -EOPNOTSUPP;

	for (i = 0; i < num; i++) {
		if (msgs[i].len == 0) {
			dev_err(hdmi->dev,
				"unsupported transfer %d/%d, no data\n",
				i + 1, num);
			return -EOPNOTSUPP;
		}
	}

	mutex_lock(&i2c->lock);

	/* Unmute DONE and ERROR interrupts */
	dw_hdmi_qp_mod(hdmi, I2CM_NACK_RCVD_MASK_N | I2CM_OP_DONE_MASK_N,
		       I2CM_NACK_RCVD_MASK_N | I2CM_OP_DONE_MASK_N,
		       MAINUNIT_1_INT_MASK_N);

	/* Set slave device address taken from the first I2C message */
	if (addr == DDC_SEGMENT_ADDR && msgs[0].len == 1)
		addr = DDC_ADDR;

	dw_hdmi_qp_mod(hdmi, addr << 5, I2CM_SLVADDR, I2CM_INTERFACE_CONTROL0);

	/* Set slave device register address on transfer */
	i2c->is_regaddr = false;

	/* Set segment pointer for I2C extended read mode operation */
	i2c->is_segment = false;

	for (i = 0; i < num; i++) {
		if (msgs[i].addr == DDC_SEGMENT_ADDR && msgs[i].len == 1) {
			i2c->is_segment = true;
			dw_hdmi_qp_mod(hdmi, DDC_SEGMENT_ADDR, I2CM_SEG_ADDR,
				       I2CM_INTERFACE_CONTROL1);
			dw_hdmi_qp_mod(hdmi, *msgs[i].buf, I2CM_SEG_PTR,
				       I2CM_INTERFACE_CONTROL1);
		} else {
			if (msgs[i].flags & I2C_M_RD)
				ret = dw_hdmi_qp_i2c_read(hdmi, msgs[i].buf,
							  msgs[i].len);
			else
				ret = dw_hdmi_qp_i2c_write(hdmi, msgs[i].buf,
							   msgs[i].len);
		}
		if (ret < 0)
			break;
	}

	if (!ret)
		ret = num;

	/* Mute DONE and ERROR interrupts */
	dw_hdmi_qp_mod(hdmi, 0, I2CM_OP_DONE_MASK_N | I2CM_NACK_RCVD_MASK_N,
		       MAINUNIT_1_INT_MASK_N);

	mutex_unlock(&i2c->lock);

	return ret;
}

static u32 dw_hdmi_qp_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm dw_hdmi_algorithm = {
	.master_xfer	= dw_hdmi_qp_i2c_xfer,
	.functionality	= dw_hdmi_qp_i2c_func,
};

static void hdmi_infoframe_set_checksum(u8 *ptr, int size)
{
	u8 csum = 0;
	int i;

	ptr[3] = 0;
	/* compute checksum */
	for (i = 0; i < size; i++)
		csum += ptr[i];

	ptr[3] = 256 - csum;
}

static void hdmi_config_AVI(struct dw_hdmi *hdmi,
			    const struct drm_connector *connector,
			    const struct drm_display_mode *mode)
{
	struct hdmi_avi_infoframe frame;
	u32 val, i, j;
	u8 buf[17];

	dw_hdmi_prep_avi_infoframe(&frame, hdmi, connector, mode);

	frame.scan_mode = HDMI_SCAN_MODE_NONE;
	frame.video_code = hdmi->vic;

	hdmi_avi_infoframe_pack_only(&frame, buf, 17);

	/* mode which vic >= 128 must use avi version 3 */
	if (hdmi->vic >= 128) {
		frame.version = 3;
		buf[1] = frame.version;
		buf[4] &= 0x1f;
		buf[4] |= ((frame.colorspace & 0x7) << 5);
		buf[7] = frame.video_code;
		hdmi_infoframe_set_checksum(buf, 17);
	}

	/*
	 * The Designware IP uses a different byte format from standard
	 * AVI info frames, though generally the bits are in the correct
	 * bytes.
	 */

	val = (frame.version << 8) | (frame.length << 16);
	dw_hdmi_qp_write(hdmi, val, PKT_AVI_CONTENTS0);

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			if (i * 4 + j >= 14)
				break;
			if (!j)
				val = buf[i * 4 + j + 3];
			val |= buf[i * 4 + j + 3] << (8 * j);
		}

		dw_hdmi_qp_write(hdmi, val, PKT_AVI_CONTENTS1 + i * 4);
	}

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_AVI_FIELDRATE, PKTSCHED_PKT_CONFIG1);

	dw_hdmi_qp_mod(hdmi, PKTSCHED_AVI_TX_EN | PKTSCHED_GCP_TX_EN,
		       PKTSCHED_AVI_TX_EN | PKTSCHED_GCP_TX_EN, PKTSCHED_PKT_EN);
}

static void hdmi_config_drm_infoframe(struct dw_hdmi *hdmi,
				      const struct drm_connector *connector)
{
	const struct drm_connector_state *conn_state = connector->state;
	struct hdr_output_metadata *hdr_metadata;
	struct hdmi_drm_infoframe frame;
	u8 buffer[30];
	ssize_t err;
	int i;
	u32 val;

	if (!hdmi->plat_data->use_drm_infoframe)
		return;

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_DRMI_TX_EN, PKTSCHED_PKT_EN);

	if (!hdmi->connector.hdr_sink_metadata.hdmi_type1.eotf) {
		dev_dbg(hdmi->dev, "No need to set HDR metadata in infoframe\n");
		return;
	}

	if (!conn_state->hdr_output_metadata) {
		dev_dbg(hdmi->dev, "source metadata not set yet\n");
		return;
	}

	hdr_metadata = (struct hdr_output_metadata *)
		conn_state->hdr_output_metadata->data;

	if (!(hdmi->connector.hdr_sink_metadata.hdmi_type1.eotf &
	      BIT(hdr_metadata->hdmi_metadata_type1.eotf))) {
		dev_err(hdmi->dev, "EOTF %d not supported\n",
			hdr_metadata->hdmi_metadata_type1.eotf);
		return;
	}

	err = drm_hdmi_infoframe_set_hdr_metadata(&frame, conn_state);
	if (err < 0)
		return;

	err = hdmi_drm_infoframe_pack(&frame, buffer, sizeof(buffer));
	if (err < 0) {
		dev_err(hdmi->dev, "Failed to pack drm infoframe: %zd\n", err);
		return;
	}

	val = (frame.version << 8) | (frame.length << 16);
	dw_hdmi_qp_write(hdmi, val, PKT_DRMI_CONTENTS0);

	for (i = 0; i <= frame.length; i++) {
		if (i % 4 == 0)
			val = buffer[3 + i];
		val |= buffer[3 + i] << ((i % 4) * 8);

		if ((i % 4 == 3) || i == frame.length)
			dw_hdmi_qp_write(hdmi, val,
					 PKT_DRMI_CONTENTS1 + ((i / 4) * 4));
	}

	dw_hdmi_qp_mod(hdmi, 0, PKTSCHED_DRMI_FIELDRATE, PKTSCHED_PKT_CONFIG1);
	dw_hdmi_qp_mod(hdmi, PKTSCHED_DRMI_TX_EN, PKTSCHED_DRMI_TX_EN,
		       PKTSCHED_PKT_EN);
}

static int dw_hdmi_qp_setup(struct dw_hdmi *hdmi,
			    struct drm_connector *connector,
			    struct drm_display_mode *mode)
{
	u8 bytes = 0;
	int ret;

	dw_hdmi_prep_data(hdmi, mode);

	if (mode->flags & DRM_MODE_FLAG_DBLCLK) {
		hdmi->hdmi_data.video_mode.mpixelrepetitionoutput = 1;
		hdmi->hdmi_data.video_mode.mpixelrepetitioninput = 1;
	}

	/*
	 * According to the dw-hdmi specification 6.4.2
	 * vp_pr_cd[3:0]:
	 * 0000b: No pixel repetition (pixel sent only once)
	 * 0001b: Pixel sent two times (pixel repeated once)
	 */
	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		hdmi->hdmi_data.pix_repet_factor = 1;

	/* HDMI Initialization Step B.1 */
	dw_hdmi_prep_vmode(hdmi, mode);

	/* HDMI Initialization Step B.2 */
	ret = hdmi->phy.ops->init(hdmi, hdmi->phy.data,
				  &connector->display_info,
				  &hdmi->previous_mode);
	if (ret)
		return ret;
	hdmi->phy.enabled = true;

	/* not for DVI mode */
	if (hdmi->sink_is_hdmi) {
		dev_dbg(hdmi->dev, "%s HDMI mode\n", __func__);

		dw_hdmi_qp_mod(hdmi, 0, OPMODE_DVI, LINK_CONFIG0);
		dw_hdmi_qp_mod(hdmi, HDCP2_BYPASS, HDCP2_BYPASS, HDCP2LOGIC_CONFIG0);

		if (hdmi->hdmi_data.video_mode.mtmdsclock > HDMI14_MAX_TMDSCLK) {
			if (dw_hdmi_support_scdc(hdmi, &connector->display_info)) {
				drm_scdc_readb(hdmi->ddc, SCDC_SINK_VERSION, &bytes);
				drm_scdc_writeb(hdmi->ddc, SCDC_SOURCE_VERSION,
						min_t(u8, bytes, SCDC_MIN_SOURCE_VERSION));
				drm_scdc_set_high_tmds_clock_ratio(connector, 1);
				drm_scdc_set_scrambling(connector, 1);
			}
			dw_hdmi_qp_write(hdmi, 1, SCRAMB_CONFIG0);
		} else {
			if (dw_hdmi_support_scdc(hdmi, &connector->display_info)) {
				drm_scdc_set_high_tmds_clock_ratio(connector, 0);
				drm_scdc_set_scrambling(connector, 0);
			}
			dw_hdmi_qp_write(hdmi, 0, SCRAMB_CONFIG0);
		}

		/* HDMI Initialization Step F */
		hdmi_config_AVI(hdmi, connector, mode);
		hdmi_config_drm_infoframe(hdmi, connector);
	} else {
		dev_dbg(hdmi->dev, "%s DVI mode\n", __func__);

		dw_hdmi_qp_mod(hdmi, HDCP2_BYPASS, HDCP2_BYPASS, HDCP2LOGIC_CONFIG0);
		dw_hdmi_qp_mod(hdmi, OPMODE_DVI, OPMODE_DVI, LINK_CONFIG0);
	}

	return 0;
}

static void dw_hdmi_qp_update_power(struct dw_hdmi *hdmi)
{
	int force = hdmi->force;

	if (hdmi->disabled) {
		force = DRM_FORCE_OFF;
	} else if (force == DRM_FORCE_UNSPECIFIED) {
		if (hdmi->rxsense)
			force = DRM_FORCE_ON;
		else
			force = DRM_FORCE_OFF;
	}

	if (force == DRM_FORCE_OFF) {
		if (hdmi->bridge_is_on) {
			if (hdmi->phy.enabled) {
				hdmi->phy.ops->disable(hdmi, hdmi->phy.data);
				hdmi->phy.enabled = false;
			}

			hdmi->bridge_is_on = false;
		}
	} else {
		if (!hdmi->bridge_is_on) {
			hdmi->bridge_is_on = true;

			/*
			 * The curr_conn field is guaranteed to be valid here, as this function
			 * is only be called when !hdmi->disabled.
			 */
			dw_hdmi_qp_setup(hdmi, hdmi->curr_conn, &hdmi->previous_mode);
		}
	}
}

static void dw_hdmi_qp_connector_force(struct drm_connector *connector)
{
	struct dw_hdmi *hdmi =
		container_of(connector, struct dw_hdmi, connector);

	mutex_lock(&hdmi->mutex);
	hdmi->force = connector->force;
	dw_hdmi_qp_update_power(hdmi);
	mutex_unlock(&hdmi->mutex);
}

static const struct drm_connector_funcs dw_hdmi_qp_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = dw_hdmi_connector_detect,
	.destroy = drm_connector_cleanup,
	.force = dw_hdmi_qp_connector_force,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int dw_hdmi_qp_bridge_attach(struct drm_bridge *bridge,
				    enum drm_bridge_attach_flags flags)
{
	struct dw_hdmi *hdmi = bridge->driver_private;

	if (flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)
		return drm_bridge_attach(bridge->encoder, hdmi->next_bridge,
					 bridge, flags);

	return dw_hdmi_connector_create(hdmi, &dw_hdmi_qp_connector_funcs);
}

static enum drm_mode_status
dw_hdmi_qp_bridge_mode_valid(struct drm_bridge *bridge,
			     const struct drm_display_info *info,
			     const struct drm_display_mode *mode)
{
	struct dw_hdmi *hdmi = bridge->driver_private;
	const struct dw_hdmi_plat_data *pdata = hdmi->plat_data;
	enum drm_mode_status mode_status = MODE_OK;

	if (pdata->mode_valid)
		mode_status = pdata->mode_valid(hdmi, pdata->priv_data, info,
						mode);

	return mode_status;
}

static void dw_hdmi_qp_bridge_atomic_disable(struct drm_bridge *bridge,
					     struct drm_bridge_state *old_state)
{
	struct dw_hdmi *hdmi = bridge->driver_private;

	mutex_lock(&hdmi->mutex);
	hdmi->disabled = true;
	hdmi->curr_conn = NULL;
	dw_hdmi_qp_update_power(hdmi);
	dw_handle_plugged_change(hdmi, false);
	mutex_unlock(&hdmi->mutex);
}

static void dw_hdmi_qp_bridge_atomic_enable(struct drm_bridge *bridge,
					    struct drm_bridge_state *old_state)
{
	struct dw_hdmi *hdmi = bridge->driver_private;
	struct drm_atomic_state *state = old_state->base.state;
	struct drm_connector *connector;

	connector = drm_atomic_get_new_connector_for_encoder(state,
							     bridge->encoder);

	mutex_lock(&hdmi->mutex);
	hdmi->disabled = false;
	hdmi->curr_conn = connector;
	dw_hdmi_qp_update_power(hdmi);
	dw_handle_plugged_change(hdmi, true);
	mutex_unlock(&hdmi->mutex);
}

static const struct drm_bridge_funcs dw_hdmi_qp_bridge_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.attach = dw_hdmi_qp_bridge_attach,
	.detach = dw_hdmi_bridge_detach,
	.atomic_check = dw_hdmi_bridge_atomic_check,
	.atomic_enable = dw_hdmi_qp_bridge_atomic_enable,
	.atomic_disable = dw_hdmi_qp_bridge_atomic_disable,
	.mode_set = dw_hdmi_bridge_mode_set,
	.mode_valid = dw_hdmi_qp_bridge_mode_valid,
	.detect = dw_hdmi_bridge_detect,
	.edid_read = dw_hdmi_bridge_edid_read,
};

static irqreturn_t dw_hdmi_qp_main_hardirq(int irq, void *dev_id)
{
	struct dw_hdmi *hdmi = dev_id;
	struct dw_hdmi_i2c *i2c = hdmi->i2c;
	u32 stat;

	stat = dw_hdmi_qp_read(hdmi, MAINUNIT_1_INT_STATUS);

	i2c->stat = stat & (I2CM_OP_DONE_IRQ | I2CM_READ_REQUEST_IRQ |
			    I2CM_NACK_RCVD_IRQ);

	if (i2c->stat) {
		dw_hdmi_qp_write(hdmi, i2c->stat, MAINUNIT_1_INT_CLEAR);
		complete(&i2c->cmp);
	}

	if (stat)
		return IRQ_HANDLED;

	return IRQ_NONE;
}

static int dw_hdmi_qp_detect_phy(struct dw_hdmi *hdmi)
{
	if (!hdmi->plat_data->phy_force_vendor) {
		dev_err(hdmi->dev, "Internal HDMI PHY not supported\n");
		return -ENODEV;
	}

	/* Vendor PHYs require support from the glue layer. */
	if (!hdmi->plat_data->phy_ops || !hdmi->plat_data->phy_name) {
		dev_err(hdmi->dev,
			"Vendor HDMI PHY not supported by glue layer\n");
		return -ENODEV;
	}

	hdmi->phy.ops = hdmi->plat_data->phy_ops;
	hdmi->phy.data = hdmi->plat_data->phy_data;
	hdmi->phy.name = hdmi->plat_data->phy_name;

	return 0;
}

static const struct regmap_config dw_hdmi_qp_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= EARCRX_1_INT_FORCE,
};

static void dw_hdmi_qp_init_hw(struct dw_hdmi *hdmi)
{
	dw_hdmi_qp_write(hdmi, 0, MAINUNIT_0_INT_MASK_N);
	dw_hdmi_qp_write(hdmi, 0, MAINUNIT_1_INT_MASK_N);
	dw_hdmi_qp_write(hdmi, 428571429, TIMER_BASE_CONFIG0);

	dw_hdmi_qp_i2c_init(hdmi);

	if (hdmi->phy.ops->setup_hpd)
		hdmi->phy.ops->setup_hpd(hdmi, hdmi->phy.data);
}

static struct dw_hdmi *
dw_hdmi_qp_probe(struct platform_device *pdev,
		 const struct dw_hdmi_plat_data *plat_data)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *ddc_node;
	struct dw_hdmi *hdmi;
	struct resource *iores = NULL;
	int irq, ret;

	hdmi = devm_kzalloc(dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return ERR_PTR(-ENOMEM);

	hdmi->plat_data = plat_data;
	hdmi->dev = dev;
	hdmi->disabled = true;
	hdmi->rxsense = true;
	hdmi->last_connector_result = connector_status_disconnected;

	mutex_init(&hdmi->mutex);
	mutex_init(&hdmi->audio_mutex);
	mutex_init(&hdmi->cec_notifier_mutex);
	spin_lock_init(&hdmi->audio_lock);

	ddc_node = of_parse_phandle(np, "ddc-i2c-bus", 0);
	if (ddc_node) {
		hdmi->ddc = of_get_i2c_adapter_by_node(ddc_node);
		of_node_put(ddc_node);
		if (!hdmi->ddc) {
			dev_dbg(hdmi->dev, "failed to read ddc node\n");
			return ERR_PTR(-EPROBE_DEFER);
		}

	} else {
		dev_dbg(hdmi->dev, "no ddc property found\n");
	}

	if (!plat_data->regm) {
		const struct regmap_config *reg_config;

		reg_config = &dw_hdmi_qp_regmap_config;

		iores = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		hdmi->regs = devm_ioremap_resource(dev, iores);
		if (IS_ERR(hdmi->regs)) {
			ret = PTR_ERR(hdmi->regs);
			goto err_put;
		}

		hdmi->regm = devm_regmap_init_mmio(dev, hdmi->regs, reg_config);
		if (IS_ERR(hdmi->regm)) {
			dev_err(dev, "Failed to configure regmap\n");
			ret = PTR_ERR(hdmi->regm);
			goto err_put;
		}
	} else {
		hdmi->regm = plat_data->regm;
	}

	/* Allow SCDC advertising in dw_hdmi_support_scdc() */
	hdmi->version = 0x200a;

	ret = dw_hdmi_qp_detect_phy(hdmi);
	if (ret < 0)
		goto err_put;

	dw_hdmi_qp_init_hw(hdmi);

	if ((dw_hdmi_qp_read(hdmi, CMU_STATUS) & DISPLAY_CLK_MONITOR) ==
			DISPLAY_CLK_LOCKED)
		hdmi->disabled = false;

	/* Not handled for now: IRQ0 (AVP), IRQ1 (CEC), IRQ2 (EARC) */
	irq = platform_get_irq(pdev, 3);
	if (irq < 0) {
		ret = irq;
		goto err_put;
	}

	ret = devm_request_threaded_irq(dev, irq,
					dw_hdmi_qp_main_hardirq, NULL,
					IRQF_SHARED, dev_name(dev), hdmi);
	if (ret)
		goto err_put;

	/* If DDC bus is not specified, try to register HDMI I2C bus */
	if (!hdmi->ddc) {
		hdmi->ddc = dw_hdmi_i2c_adapter(hdmi, &dw_hdmi_algorithm);
		if (IS_ERR(hdmi->ddc))
			hdmi->ddc = NULL;
	}

	hdmi->bridge.driver_private = hdmi;
	hdmi->bridge.funcs = &dw_hdmi_qp_bridge_funcs;
	hdmi->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID
			 | DRM_BRIDGE_OP_HPD;
	hdmi->bridge.ddc = hdmi->ddc;
	hdmi->bridge.of_node = pdev->dev.of_node;
	hdmi->bridge.type = DRM_MODE_CONNECTOR_HDMIA;

	drm_bridge_add(&hdmi->bridge);

	return hdmi;

err_put:
	i2c_put_adapter(hdmi->ddc);

	return ERR_PTR(ret);
}

static void dw_hdmi_qp_remove(struct dw_hdmi *hdmi)
{
	drm_bridge_remove(&hdmi->bridge);

	if (hdmi->audio && !IS_ERR(hdmi->audio))
		platform_device_unregister(hdmi->audio);
	if (!IS_ERR(hdmi->cec))
		platform_device_unregister(hdmi->cec);

	if (hdmi->i2c)
		i2c_del_adapter(&hdmi->i2c->adap);
	else
		i2c_put_adapter(hdmi->ddc);
}

struct dw_hdmi *dw_hdmi_qp_bind(struct platform_device *pdev,
				struct drm_encoder *encoder,
				struct dw_hdmi_plat_data *plat_data)
{
	struct dw_hdmi *hdmi;
	int ret;

	hdmi = dw_hdmi_qp_probe(pdev, plat_data);
	if (IS_ERR(hdmi))
		return hdmi;

	ret = drm_bridge_attach(encoder, &hdmi->bridge, NULL, 0);
	if (ret) {
		dw_hdmi_qp_remove(hdmi);
		return ERR_PTR(ret);
	}

	return hdmi;
}
EXPORT_SYMBOL_GPL(dw_hdmi_qp_bind);

void dw_hdmi_qp_unbind(struct dw_hdmi *hdmi)
{
	dw_hdmi_qp_remove(hdmi);
}
EXPORT_SYMBOL_GPL(dw_hdmi_qp_unbind);

void dw_hdmi_qp_resume(struct device *dev, struct dw_hdmi *hdmi)
{
	dw_hdmi_qp_init_hw(hdmi);
}
EXPORT_SYMBOL_GPL(dw_hdmi_qp_resume);

MODULE_AUTHOR("Algea Cao <algea.cao@rock-chips.com>");
MODULE_AUTHOR("Cristian Ciocaltea <cristian.ciocaltea@collabora.com>");
MODULE_DESCRIPTION("DW HDMI QP transmitter driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:dw-hdmi-qp");
