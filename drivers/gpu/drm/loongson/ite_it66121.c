// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2020 BayLibre, SAS
 * Author: Phong LE <ple@baylibre.com>
 * Copyright (C) 2018-2019, Artem Mygaiev
 * Copyright (C) 2017, Fresco Logic, Incorporated.
 *
 * IT66121 HDMI transmitter driver
 */

#include <linux/media-bus-format.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/bitfield.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "ite_it66121.h"
#include "ite_it66121_regs.h"

#define IT66121_CHIP_NAME                       "IT66121"

struct it66121_bridge {
	struct drm_bridge bridge;
	struct drm_connector connector;
	struct regmap *regmap;
	struct i2c_client *client;
	/* Protects fields below and device registers */
	struct mutex lock;
	u16 vendor_id;
	u16 device_id;
	u32 revision;
};

static inline struct it66121_bridge *
bridge_to_it66121(struct drm_bridge *bridge)
{
	return container_of(bridge, struct it66121_bridge, bridge);
}

static inline struct it66121_bridge *
connector_to_it66121(struct drm_connector *connector)
{
	return container_of(connector, struct it66121_bridge, connector);
}

static const struct regmap_range_cfg it66121_regmap_banks[] = {
	{
		.name = IT66121_CHIP_NAME,
		.range_min = 0x00,
		.range_max = 0x1FF,
		.selector_reg = IT66121_CLK_BANK_REG,
		.selector_mask = 0x1,
		.selector_shift = 0,
		.window_start = 0x00,
		.window_len = 0x100,
	},
};

static const struct regmap_config it66121_regmap_config = {
	.val_bits = 8,
	.reg_bits = 8,
	.max_register = 0x1FF,
	.ranges = it66121_regmap_banks,
	.num_ranges = ARRAY_SIZE(it66121_regmap_banks),
};

static inline int it66121_preamble_ddc(struct it66121_bridge *itb)
{
	return regmap_write(itb->regmap, IT66121_MASTER_SEL_REG,
			    IT66121_MASTER_SEL_HOST);
}

static inline int it66121_fire_afe(struct it66121_bridge *itb)
{
	return regmap_write(itb->regmap, IT66121_AFE_DRV_REG, 0);
}

static int it66121_configure_input(struct it66121_bridge *itb)
{
	int ret;

	ret = regmap_write(itb->regmap, IT66121_INPUT_MODE_REG,
			   IT66121_INPUT_MODE_RGB888);
	if (ret)
		return ret;

	return regmap_write(itb->regmap, IT66121_INPUT_CSC_REG,
			    IT66121_INPUT_CSC_NO_CONV);
}

/*
 * it66121_configure_afe() - Configure the analog front end
 * @ctx: it66121_ctx object
 * @mode: mode to configure
 *
 * RETURNS:
 * zero if success, a negative error code otherwise.
 */
static int it66121_configure_afe(struct it66121_bridge *itb,
				 const struct drm_display_mode *mode)
{
	int ret;

	ret = regmap_write(itb->regmap, IT66121_AFE_DRV_REG,
			   IT66121_AFE_DRV_RST);
	if (ret)
		return ret;

	if (mode->clock > IT66121_AFE_CLK_HIGH) {
		ret = regmap_write_bits(itb->regmap, IT66121_AFE_XP_REG,
					IT66121_AFE_XP_GAINBIT |
					IT66121_AFE_XP_ENO,
					IT66121_AFE_XP_GAINBIT);
		if (ret)
			return ret;

		ret = regmap_write_bits(itb->regmap, IT66121_AFE_IP_REG,
					IT66121_AFE_IP_GAINBIT |
					IT66121_AFE_IP_ER0,
					IT66121_AFE_IP_GAINBIT);
		if (ret)
			return ret;

		ret = regmap_write_bits(itb->regmap, IT66121_AFE_IP_REG,
					IT66121_AFE_IP_EC1, 0);
		if (ret)
			return ret;

		ret = regmap_write_bits(itb->regmap, IT66121_AFE_XP_EC1_REG,
					IT66121_AFE_XP_EC1_LOWCLK, 0x80);
		if (ret)
			return ret;
	} else {
		ret = regmap_write_bits(itb->regmap, IT66121_AFE_XP_REG,
					IT66121_AFE_XP_GAINBIT |
					IT66121_AFE_XP_ENO,
					IT66121_AFE_XP_ENO);
		if (ret)
			return ret;

		ret = regmap_write_bits(itb->regmap, IT66121_AFE_IP_REG,
					IT66121_AFE_IP_GAINBIT |
					IT66121_AFE_IP_ER0,
					IT66121_AFE_IP_ER0);
		if (ret)
			return ret;

		ret = regmap_write_bits(itb->regmap, IT66121_AFE_IP_REG,
					IT66121_AFE_IP_EC1,
					IT66121_AFE_IP_EC1);
		if (ret)
			return ret;

		ret = regmap_write_bits(itb->regmap, IT66121_AFE_XP_EC1_REG,
					IT66121_AFE_XP_EC1_LOWCLK,
					IT66121_AFE_XP_EC1_LOWCLK);
		if (ret)
			return ret;
	}

	/* Clear reset flags */
	ret = regmap_write_bits(itb->regmap, IT66121_SW_RST_REG,
				IT66121_SW_RST_REF | IT66121_SW_RST_VID, 0);
	if (ret)
		return ret;

	return it66121_fire_afe(itb);
}

static inline int it66121_wait_ddc_ready(struct it66121_bridge *itb)
{
	u32 error = IT66121_DDC_STATUS_NOACK |
		    IT66121_DDC_STATUS_WAIT_BUS |
		    IT66121_DDC_STATUS_ARBI_LOSE;
	u32 done = IT66121_DDC_STATUS_TX_DONE;
	int ret, val;

	ret = regmap_read_poll_timeout(itb->regmap, IT66121_DDC_STATUS_REG,
				       val, val & (error | done),
				       IT66121_EDID_SLEEP_US,
				       IT66121_EDID_TIMEOUT_US);
	if (ret)
		return ret;

	if (val & error)
		return -EAGAIN;

	return 0;
}

static int it66121_abort_ddc_ops(struct it66121_bridge *itb)
{
	unsigned int swreset, cpdesire;
	int ret;

	ret = regmap_read(itb->regmap, IT66121_SW_RST_REG, &swreset);
	if (ret)
		return ret;

	ret = regmap_read(itb->regmap, IT66121_HDCP_REG, &cpdesire);
	if (ret)
		return ret;

	ret = regmap_write(itb->regmap, IT66121_HDCP_REG,
			   cpdesire & (~IT66121_HDCP_CPDESIRED & 0xFF));
	if (ret)
		return ret;

	ret = regmap_write(itb->regmap, IT66121_SW_RST_REG,
			   (swreset | IT66121_SW_RST_HDCP));
	if (ret)
		return ret;

	ret = it66121_preamble_ddc(itb);
	if (ret)
		return ret;

	ret = regmap_write(itb->regmap, IT66121_DDC_COMMAND_REG,
			   IT66121_DDC_COMMAND_ABORT);
	if (ret)
		return ret;

	return it66121_wait_ddc_ready(itb);
}

static int it66121_get_edid_block(void *context,
				  u8 *buf,
				  unsigned int block,
				  size_t len)
{
	struct it66121_bridge *itb = (struct it66121_bridge *)context;
	int remain = len;
	int offset = 0;
	int ret, cnt;

	offset = (block % 2) * len;
	block = block / 2;

	while (remain > 0) {
		cnt = (remain > IT66121_EDID_FIFO_SIZE) ?
				IT66121_EDID_FIFO_SIZE : remain;

		ret = regmap_write(itb->regmap, IT66121_DDC_COMMAND_REG,
				   IT66121_DDC_COMMAND_FIFO_CLR);
		if (ret)
			return ret;

		ret = it66121_wait_ddc_ready(itb);
		if (ret)
			return ret;

		ret = regmap_write(itb->regmap, IT66121_DDC_OFFSET_REG, offset);
		if (ret)
			return ret;

		ret = regmap_write(itb->regmap, IT66121_DDC_BYTE_REG, cnt);
		if (ret)
			return ret;

		ret = regmap_write(itb->regmap, IT66121_DDC_SEGMENT_REG, block);
		if (ret)
			return ret;

		ret = regmap_write(itb->regmap, IT66121_DDC_COMMAND_REG,
				   IT66121_DDC_COMMAND_EDID_READ);
		if (ret)
			return ret;

		offset += cnt;
		remain -= cnt;

		ret = it66121_wait_ddc_ready(itb);
		if (ret) {
			it66121_abort_ddc_ops(itb);
			return ret;
		}

		ret = regmap_noinc_read(itb->regmap, IT66121_DDC_RD_FIFO_REG,
					buf, cnt);
		if (ret)
			return ret;

		buf += cnt;
	}

	return 0;
}

static bool it66121_is_hpd_detect(struct it66121_bridge *itb)
{
	int val;

	if (regmap_read(itb->regmap, IT66121_SYS_STATUS_REG, &val))
		return false;

	return val & IT66121_SYS_STATUS_HPDETECT;
}

static int it66121_connector_get_modes(struct drm_connector *connector)
{
	struct it66121_bridge *itb = connector_to_it66121(connector);
	u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
	int num_modes = 0;
	struct edid *edid;
	int ret;

	edid = drm_bridge_get_edid(&itb->bridge, connector);
	if (!edid) {
		drm_err(connector->dev, "Failed to read EDID\n");
		goto failed;
	}

	if (drm_connector_update_edid_property(connector, edid)) {
		drm_err(connector->dev, "Failed to update EDID\n");
		goto failed;
	}

	ret = drm_display_info_set_bus_formats(&connector->display_info,
					       &bus_format, 1);
	if (ret)
		goto failed;

	num_modes = drm_add_edid_modes(connector, edid);

failed:
	return num_modes;
}

static int it66121_connector_detect_ctx(struct drm_connector *connector,
					struct drm_modeset_acquire_ctx *ctx,
					bool force)
{
	struct it66121_bridge *itb = connector_to_it66121(connector);

	return it66121_is_hpd_detect(itb) ? connector_status_connected
					  : connector_status_disconnected;
}

static struct drm_connector_helper_funcs it66121_connector_helper_funcs = {
	.get_modes = it66121_connector_get_modes,
	.detect_ctx = it66121_connector_detect_ctx,
};

static const struct drm_connector_funcs it66121_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int it66121_bridge_connector_init(struct drm_bridge *bridge)
{
	struct it66121_bridge *itb = bridge_to_it66121(bridge);
	struct drm_connector *connector = &itb->connector;
	int ret;

	if (bridge->ops & DRM_BRIDGE_OP_HPD) {
		connector->polled = DRM_CONNECTOR_POLL_HPD;
	} else {
		connector->polled = DRM_CONNECTOR_POLL_CONNECT |
				    DRM_CONNECTOR_POLL_DISCONNECT;
	}

	ret = drm_connector_init(bridge->dev,
				 connector,
				 &it66121_connector_funcs,
				 bridge->type);
	if (ret)
		return ret;

	drm_connector_helper_add(connector, &it66121_connector_helper_funcs);

	drm_connector_attach_encoder(connector, bridge->encoder);

	return 0;
}

static int it66121_bridge_attach(struct drm_bridge *bridge,
				 enum drm_bridge_attach_flags flags)
{
	struct it66121_bridge *itb = bridge_to_it66121(bridge);
	int ret;

	ret = it66121_bridge_connector_init(bridge);
	if (ret)
		return ret;

	ret = regmap_write_bits(itb->regmap, IT66121_CLK_BANK_REG,
				IT66121_CLK_BANK_PWROFF_RCLK, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(itb->regmap, IT66121_INT_REG,
				IT66121_INT_TX_CLK_OFF, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(itb->regmap, IT66121_AFE_DRV_REG,
				IT66121_AFE_DRV_PWD, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(itb->regmap, IT66121_AFE_XP_REG,
				IT66121_AFE_XP_PWDI | IT66121_AFE_XP_PWDPLL, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(itb->regmap, IT66121_AFE_IP_REG,
				IT66121_AFE_IP_PWDPLL, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(itb->regmap, IT66121_AFE_DRV_REG,
				IT66121_AFE_DRV_RST, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(itb->regmap, IT66121_AFE_XP_REG,
				IT66121_AFE_XP_RESETB, IT66121_AFE_XP_RESETB);
	if (ret)
		return ret;

	ret = regmap_write_bits(itb->regmap, IT66121_AFE_IP_REG,
				IT66121_AFE_IP_RESETB, IT66121_AFE_IP_RESETB);
	if (ret)
		return ret;

	ret = regmap_write_bits(itb->regmap, IT66121_SW_RST_REG,
				IT66121_SW_RST_REF,
				IT66121_SW_RST_REF);
	if (ret)
		return ret;

	drm_info(bridge->dev,
		 "IT66121 attached, Vendor ID: 0x%x, Device ID: 0x%x\n",
		 itb->vendor_id, itb->device_id);

	/* Per programming manual, sleep here for bridge to settle */
	msleep(50);

	return 0;
}

static void it66121_bridge_enable(struct drm_bridge *bridge,
				  struct drm_bridge_state *state)
{
	struct it66121_bridge *itb = bridge_to_it66121(bridge);
	struct regmap *regmap = itb->regmap;
	int ret;

	ret = regmap_clear_bits(regmap, IT66121_AVMUTE_REG, IT66121_AVMUTE_BIT);
	if (ret)
		drm_err(bridge->dev, "Enable it66121 bridge failed");

	regmap_write(regmap, IT66121_PKT_GEN_CTRL_REG,
		     IT66121_PKT_GEN_CTRL_ON | IT66121_PKT_GEN_CTRL_RPT);
}

static void it66121_bridge_disable(struct drm_bridge *bridge,
				   struct drm_bridge_state *bridge_state)
{
	struct it66121_bridge *itb = bridge_to_it66121(bridge);
	struct regmap *regmap = itb->regmap;
	int ret;

	ret = regmap_set_bits(regmap, IT66121_AVMUTE_REG, IT66121_AVMUTE_BIT);
	if (ret)
		drm_err(bridge->dev, "Disable it66121 bridge failed");

	regmap_write(regmap, IT66121_PKT_GEN_CTRL_REG,
		     IT66121_PKT_GEN_CTRL_ON | IT66121_PKT_GEN_CTRL_RPT);
}

static void it66121_bridge_mode_set(struct drm_bridge *bridge,
				    const struct drm_display_mode *mode,
				    const struct drm_display_mode *adj_mode)
{
	struct it66121_bridge *itb = bridge_to_it66121(bridge);
	struct hdmi_avi_infoframe avi_infoframe;
	u8 av_buf[HDMI_INFOFRAME_SIZE(AVI)];
	int ret;

	mutex_lock(&itb->lock);

	hdmi_avi_infoframe_init(&avi_infoframe);

	ret = drm_hdmi_avi_infoframe_from_display_mode(&avi_infoframe,
						       &itb->connector,
						       adj_mode);
	if (ret) {
		drm_err(bridge->dev, "Failed to setup AVI infoframe\n");
		goto unlock;
	}

	ret = hdmi_avi_infoframe_pack(&avi_infoframe, av_buf, sizeof(av_buf));
	if (ret < 0) {
		drm_err(bridge->dev, "Failed to pack infoframe\n");
		goto unlock;
	}

	/* Write new AVI infoframe packet */
	ret = regmap_bulk_write(itb->regmap, IT66121_AVIINFO_DB1_REG,
				&av_buf[HDMI_INFOFRAME_HEADER_SIZE],
				HDMI_AVI_INFOFRAME_SIZE);
	if (ret)
		goto unlock;

	if (regmap_write(itb->regmap, IT66121_AVIINFO_CSUM_REG, av_buf[3]))
		goto unlock;

	/* Enable AVI infoframe */
	if (regmap_write(itb->regmap, IT66121_AVI_INFO_PKT_REG,
			 IT66121_AVI_INFO_PKT_ON | IT66121_AVI_INFO_PKT_RPT))
		goto unlock;

	/* Set TX mode to HDMI */
	if (regmap_write(itb->regmap, IT66121_HDMI_MODE_REG, IT66121_HDMI_MODE_HDMI))
		goto unlock;

	if (regmap_write_bits(itb->regmap, IT66121_CLK_BANK_REG,
			      IT66121_CLK_BANK_PWROFF_TXCLK,
			      IT66121_CLK_BANK_PWROFF_TXCLK))
		goto unlock;

	if (it66121_configure_input(itb))
		goto unlock;

	if (it66121_configure_afe(itb, adj_mode))
		goto unlock;

	if (regmap_write_bits(itb->regmap, IT66121_CLK_BANK_REG,
			      IT66121_CLK_BANK_PWROFF_TXCLK, 0))
		goto unlock;

unlock:
	mutex_unlock(&itb->lock);
}

static enum drm_mode_status
it66121_bridge_mode_valid(struct drm_bridge *bridge,
			  const struct drm_display_info *info,
			  const struct drm_display_mode *mode)
{
	if (mode->clock > 148500)
		return MODE_CLOCK_HIGH;

	if (mode->clock < 25000)
		return MODE_CLOCK_LOW;

	return MODE_OK;
}

static struct edid *it66121_bridge_get_edid(struct drm_bridge *bridge,
					    struct drm_connector *connector)
{
	struct it66121_bridge *itb = bridge_to_it66121(bridge);
	struct edid *edid;
	int ret;

	mutex_lock(&itb->lock);
	ret = it66121_preamble_ddc(itb);
	if (ret) {
		edid = NULL;
		goto unlock;
	}

	ret = regmap_write(itb->regmap, IT66121_DDC_HEADER_REG,
			   IT66121_DDC_HEADER_EDID);
	if (ret) {
		edid = NULL;
		goto unlock;
	}

	edid = drm_do_get_edid(connector, it66121_get_edid_block, itb);

unlock:
	mutex_unlock(&itb->lock);

	return edid;
}

static void it66121_bridge_detach(struct drm_bridge *bridge)
{
	struct it66121_bridge *itb = bridge_to_it66121(bridge);

	mutex_destroy(&itb->lock);

	i2c_unregister_device(itb->client);

	drm_bridge_remove(bridge);
}

static const struct drm_bridge_funcs it66121_bridge_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.attach = it66121_bridge_attach,
	.detach = it66121_bridge_detach,
	.atomic_enable = it66121_bridge_enable,
	.atomic_disable = it66121_bridge_disable,
	.mode_set = it66121_bridge_mode_set,
	.mode_valid = it66121_bridge_mode_valid,
	.get_edid = it66121_bridge_get_edid,
};

static void it66121_bridge_get_version(struct it66121_bridge *itb)
{
	u32 vendor_ids[2] = { 0 };
	u32 device_ids[2] = { 0 };

	regmap_read(itb->regmap, IT66121_VENDOR_ID0_REG, &vendor_ids[0]);
	regmap_read(itb->regmap, IT66121_VENDOR_ID1_REG, &vendor_ids[1]);
	regmap_read(itb->regmap, IT66121_DEVICE_ID0_REG, &device_ids[0]);
	regmap_read(itb->regmap, IT66121_DEVICE_ID1_REG, &device_ids[1]);

	/* Revision is shared with DEVICE_ID1 */
	itb->revision = FIELD_GET(IT66121_REVISION_MASK, device_ids[1]);
	device_ids[1] &= IT66121_DEVICE_ID1_MASK;

	itb->vendor_id = vendor_ids[1] << 8 | vendor_ids[0];
	itb->device_id = device_ids[1] << 8 | device_ids[0];
}

static void it66121_bridge_init_base(struct it66121_bridge *itb, bool hpd)
{
	struct drm_bridge *bridge = &itb->bridge;

	bridge->funcs = &it66121_bridge_funcs;
	bridge->type = DRM_MODE_CONNECTOR_HDMIA;
	bridge->ops = DRM_BRIDGE_OP_EDID;

	if (hpd)
		bridge->ops |= DRM_BRIDGE_OP_HPD;

	drm_bridge_add(bridge);
}

/*
 * The device address is 0x98 if PCADR pin is pulled low, 0x98 >> 1 = 0x4c
 * The device address is 0x9A if PCADR pin is pulled high, 0x9A >> 1 = 0x4d
 */
static bool it66121_probe_slave(struct drm_device *ddev,
				struct i2c_adapter *adapter,
				u8 *addr)
{
	struct i2c_msg msg = {
		.len = 0,
	};
	int num = 3;
	int count;
	int i;

	/* Try slave address 0x4c */
	msg.addr = 0x4c;
	count = 0;
	for (i = 0; i < num; i++) {
		count += i2c_transfer(adapter, &msg, 1);
		udelay(9);
	}

	if (count == num) {
		*addr = 0x4c;
		return true;
	}

	/* Try slave address 0x4d */
	msg.addr = 0x4d;
	count = 0;
	for (i = 0; i < num; i++) {
		count += i2c_transfer(adapter, &msg, 1);
		udelay(9);
	}

	if (count == num) {
		*addr = 0x4d;
		return true;
	}

	drm_err(ddev, "No reliable slave i2c device found\n");

	/*
	 * If no reliable slave i2c device found, we would like drop the
	 * support.
	 */
	return false;
}

struct drm_bridge *it66121_bridge_create(struct drm_device *ddev,
					 struct i2c_adapter *i2c,
					 u8 addr,
					 bool enable_hpd,
					 u32 int_gpio,
					 unsigned int pipe)
{
	struct i2c_board_info it66121_board_info = {
		.type = IT66121_CHIP_NAME,
	};
	struct it66121_bridge *itb;
	struct i2c_client *client;
	u8 addr_probed;

	if (!it66121_probe_slave(ddev, i2c, &addr_probed))
		return NULL;

	if (addr != addr_probed) {
		drm_warn(ddev, "device address(0x%x) is not correct\n", addr);
		addr = addr_probed;
	}

	it66121_board_info.addr = addr;

	itb = devm_kzalloc(ddev->dev, sizeof(*itb), GFP_KERNEL);
	if (!itb)
		return NULL;

	client = i2c_new_client_device(i2c, &it66121_board_info);
	if (IS_ERR(client))
		return NULL;

	drm_info(ddev, "i2c client %s@0x%02x created\n",
		 it66121_board_info.type, it66121_board_info.addr);

	itb->client = client;

	i2c_set_clientdata(client, itb);

	mutex_init(&itb->lock);

	itb->regmap = devm_regmap_init_i2c(client, &it66121_regmap_config);
	if (IS_ERR(itb->regmap)) {
		drm_err(ddev, "Failed to map registers\n");
		return NULL;
	}

	it66121_bridge_get_version(itb);

	it66121_bridge_init_base(itb, enable_hpd);

	return &itb->bridge;
}
