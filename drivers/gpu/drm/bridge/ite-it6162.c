// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 ITE Tech. Inc.
 */
#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_irq.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <drm/display/drm_hdcp_helper.h>
#include <drm/display/drm_hdmi_helper.h>
#include <drm/display/drm_hdmi_state_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <video/videomode.h>
#include <sound/hdmi-codec.h>

#define DATA_BUFFER_DEPTH 32

#define OFFSET_CHIP_ID_L 0x00
#define OFFSET_CHIP_ID_M 0x01
#define OFFSET_CHIP_ID_H 0x02
#define OFFSET_VERSION_L 0x03
#define OFFSET_VERSION_M 0x04
#define OFFSET_VERSION_H 0x05

#define OFFSET_MIPI_CONFIG_L 0x06
#define MIPI_CLOCK_MODE_MASK BIT(6)
#define MIPI_PORT1_EN_MASK BIT(5)
#define MIPI_PORT0_EN_MASK BIT(4)
#define MIPI_LANE_SWAP_MASK BIT(3)
#define MIPI_PN_SWAP_MASK BIT(2)
#define MIPI_LANE_NUM_MASK 0x3
#define MIPI_PORT_EN_MASK (MIPI_PORT1_EN_MASK | MIPI_PORT0_EN_MASK)

#define OFFSET_MIPI_CONFIG_H 0x07
#define OFFSET_TX_CONFIG 0x08

#define OFFSET_TX_SETTING 0x09
#define TX_HDCP_VER_MASK 0xC0
#define TX_HDCP_ENCRYP_MASK BIT(5)
#define TX_STREAM_TYPE_MASK BIT(4)

#define OFFSET_MIPI_STATUS 0x0A
#define OFFSET_TX_STATUS 0x0C
#define B_TX_STATUS_HPD 7
#define B_TX_STATUS_VIDEO_STB 6
#define B_TX_STATUS_HDCP 4
#define M_TX_STATUS_HDCP 0x30

#define TX_VIDEO_STB BIT(B_TX_STATUS_VIDEO_STB)
#define TX_STATUS_HPD BIT(B_TX_STATUS_HPD)

#define GET_TX_HPD_STATUS(x) (((x) & TX_STATUS_HPD) >> B_TX_STATUS_HPD)
#define GET_TX_VIDEO_STATUS(x) (((x) & TX_VIDEO_STB) >> B_TX_STATUS_VIDEO_STB)
#define GET_TX_HDCP_STATUS(x) (((x) & M_TX_STATUS_HDCP) >> B_TX_STATUS_HDCP)

#define OFFSET_SINK_CAP 0x0D
#define B_SINK_CAP_HDCP_VER 4
#define M_SINK_CAP_HDCP_VER 0x30

#define GET_SINK_CAP_HDCP_VER(x) (((x) & M_SINK_CAP_HDCP_VER) >> B_SINK_CAP_HDCP_VER)

#define OFFSET_DRIVER_DATA 0x0E

#define OFFSET_DATA_TYPE_IDX 0x0F
#define OFFSET_DATA_BUFFER 0x20

#define OFFSET_AUDIO_CTRL0 0x3C
#define MASK_AUDIO_CHANNEL_NUM 0x0F
#define MASK_AUDIO_SELECT 0x30
#define MASK_AUDIO_TYPE 0xC0

#define OFFSET_AUDIO_CTRL1 0x3D
#define OFFSET_AUDIO_CTRL2 0x3E
#define MASK_I2S_BUS_MODE 0x1F
#define MASK_I2S_WORD_LEN 0x60
#define MASK_I2S_VALID 0x80

#define OFFSET_HOST_SET 0xFE
#define B_CONFIG_CHG BIT(7)
#define B_SET_CHG BIT(6)
#define HOST_SET_VIDEO_INFO (1)
#define HOST_SET_AUDIO_INFO (2)
#define HOST_SET_VIDEO_AUDIO_INFO (3)
#define HOST_SET_EDID_R (0x04)
#define HOST_SET_HDCP_R (0x05)
#define HOST_SET_CEA_INFOFRAME (0x0E)

#define OFFSET_EVENT_CHG 0xFF
#define B_EVENT_CHG_BUFFER 4
#define M_EVENT_CHG_BUFFER_STS (0x30)

#define B_EVENT_CHG 1
#define B_EVENT_IC_READY 0

#define EVENT_CHG BIT(B_EVENT_CHG)
#define EVENT_READY BIT(B_EVENT_IC_READY)

#define GET_BUFFER_STATUS(x) (((x) & M_EVENT_CHG_BUFFER_STS) >> B_EVENT_CHG_BUFFER)

#define TIMEOUT_INFOBLOCK_MS 1800

enum it6162_audio_select {
	I2S = 0,
	SPDIF,
};

enum it6162_audio_word_length {
	WORD_LENGTH_16BIT = 0x0,
	WORD_LENGTH_18BIT = 0x1,
	WORD_LENGTH_20BIT = 0x2,
	WORD_LENGTH_24BIT = 0x3,
};

enum it6162_audio_sample_rate {
	SAMPLE_RATE_32K = 0x3,
	SAMPLE_RATE_48K = 0x2,
	SAMPLE_RATE_64K = 0xB,
	SAMPLE_RATE_96K = 0xA,
	SAMPLE_RATE_192K = 0xE,
	SAMPLE_RATE_44_1K = 0x0,
	SAMPLE_RATE_88_2K = 0x8,
	SAMPLE_RATE_176_4K = 0xC,
};

enum it6162_audio_type {
	LPCM = 0,
	NLPCM,
};

enum data_buf_sts {
	NO_STS = 0x00,
	BUF_READY = 0x01,
	BUF_FAIL = 0x02,
};

enum hdcp_state {
	HDCP_STATE_IDLE = 0x00,
	HDCP_STATE_AUTH_DONE = 0x01,
	HDCP_STATE_AUTH_FAIL = 0x02,
};

enum hdcp_ver {
	HDCP_DISABLE = 0x0,
	HDCP_14 = 0x1,
	HDCP_23 = 0x2,
	HDCP_AUTO = 0x3,
};

struct it6162_chip_info {
	u32 chip_id;
	u32 version;
};

struct it6162_video_settings {
	u8 vic;
	u32 clock;
	u16 htotal;
	u16 hfp;
	u16 hsw;
	u16 hbp;
	u16 hdew;
	u16 vtotal;
	u16 vfp;
	u16 vsw;
	u16 vbp;
	u16 vdew;
	u8 hpol;
	u8 vpol;
	u8 prog;
	u16 v_aspect;
	u16 h_aspect;
	u8 pix_rep;
	u8 colorspace;
};

enum sync_mode {
	SYNC_EVENT = 0x0,
	SYNC_PULSE = 0x1,
	SYNC_AUTO = 0x2,
};

struct it6162_mipi_cfg {
	bool en_port[2];
	u8 lane_num;
	bool pn_swap;
	bool lane_swap;
	bool continuous_clk;
	enum sync_mode mode;
	enum mipi_dsi_pixel_format format;
	unsigned long mode_flags;
};

struct it6162_hdcp_cfg {
	enum hdcp_ver hdcp_version;
	bool hdcp_encryption;
	u8 stream_ID;
};

struct it6162_infoblock_msg {
	u8 action;
	int len;
	u8 msg[32];
};

struct it6162 {
	struct drm_bridge bridge;
	struct device *dev;
	struct drm_connector *connector;
	enum drm_connector_status connector_status;
	struct drm_device *drm;

	struct i2c_client *it6162_i2c;
	struct regmap *regmap;

	struct regulator *pwr1833;
	struct regulator *ovdd;
	struct regulator *ivdd;
	struct gpio_desc *gpiod_reset;

	bool power_on;
	bool en_hdcp;

	/*lock the infoblock write setting and buffer access*/
	struct mutex lock;

	enum data_buf_sts data_buf_sts;
	wait_queue_head_t data_buf_wait;

	/* it6162 DSI RX related params */
	struct mipi_dsi_device *dsi;
	struct it6162_mipi_cfg mipi_cfg;

	struct delayed_work hdcp_work;
	enum hdcp_state hdcp_sts;
	enum hdcp_ver hdcp_version;
	struct it6162_hdcp_cfg hdcp_cfg;

	unsigned int content_protection;
};

static struct it6162 *bridge_to_it6162(struct drm_bridge *bridge)
{
	return container_of(bridge, struct it6162, bridge);
}

static const struct regmap_config it6162_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static inline int it6162_i2c_regmap_init(struct i2c_client *client,
					 struct it6162 *it6162)
{
	it6162->it6162_i2c = client;
	i2c_set_clientdata(client, it6162);
	it6162->regmap = devm_regmap_init_i2c(it6162->it6162_i2c,
					      &it6162_regmap_config);
	if (IS_ERR(it6162->regmap))
		return PTR_ERR(it6162->regmap);

	return 0;
}

static int it6162_wait_command_complete(struct it6162 *it6162)
{
	struct device *dev = it6162->dev;
	struct regmap *regmap = it6162->regmap;
	unsigned int val;
	int ret;

	ret = regmap_read(regmap, OFFSET_HOST_SET, &val);
	if (ret < 0) {
		dev_err(dev, "read failed rx reg[0x%x]", OFFSET_HOST_SET);
		return ret;
	}
	return val;
}

static int it6162_infoblock_trigger(struct it6162 *it6162, u8 setting)
{
	struct device *dev = it6162->dev;
	int status;
	int val;

	regmap_write(it6162->regmap, OFFSET_HOST_SET, setting);
	/* OFFSET_HOST_SET is cleared by firmware when the command is processed */
	status = readx_poll_timeout(it6162_wait_command_complete, it6162, val,
				    val <= 0, 50 * 1000,
				    TIMEOUT_INFOBLOCK_MS * 1000);
	if (status < 0 || val != 0) {
		dev_err(dev, "infoblock command timeout: status=%d val=%d",
			status, val);
		return -ETIMEDOUT;
	}

	return 0;
}

static int it6162_infoblock_request_data(struct it6162 *it6162,
					 u8 request_type, u8 index, u8 *buf)
{
	struct device *dev = it6162->dev;
	int status;

	it6162->data_buf_sts = NO_STS;
	regmap_write(it6162->regmap, OFFSET_DATA_TYPE_IDX, index);
	status = it6162_infoblock_trigger(it6162, request_type);
	if (status < 0)
		return status;
	/* data_buf_sts is updated asynchronously by the interrupt handler */
	if (wait_event_timeout(it6162->data_buf_wait,
			       it6162->data_buf_sts != NO_STS,
			       msecs_to_jiffies(TIMEOUT_INFOBLOCK_MS)) == 0) {
		dev_err(dev, "timeout waiting for data buffer");
		return -ETIMEDOUT;
	}

	if (it6162->data_buf_sts != BUF_READY) {
		dev_err(dev, "data buffer not ready: sts=%d",
			it6162->data_buf_sts);
		return -EIO;
	}

	regmap_bulk_read(it6162->regmap, OFFSET_DATA_BUFFER, buf,
			 DATA_BUFFER_DEPTH);
	return 0;
}

static void it6162_mipi_disable(struct it6162 *it6162)
{
	guard(mutex)(&it6162->lock);
	regmap_update_bits(it6162->regmap, OFFSET_MIPI_CONFIG_L,
			   MIPI_PORT_EN_MASK, 0x00);
	it6162_infoblock_trigger(it6162, B_CONFIG_CHG);
}

static void it6162_mipi_enable(struct it6162 *it6162)
{
	unsigned int cfg_val = 0;

	cfg_val = FIELD_PREP(MIPI_PORT1_EN_MASK, it6162->mipi_cfg.en_port[1]) |
		  FIELD_PREP(MIPI_PORT0_EN_MASK, it6162->mipi_cfg.en_port[0]);

	guard(mutex)(&it6162->lock);
	regmap_update_bits(it6162->regmap, OFFSET_MIPI_CONFIG_L,
			   MIPI_PORT_EN_MASK, cfg_val);
	it6162_infoblock_trigger(it6162, B_CONFIG_CHG);
}

static void it6162_tx_hdcp_enable(struct it6162 *it6162,
				  struct it6162_hdcp_cfg *tx_out)
{
	int val;
	struct regmap *regmap = it6162->regmap;

	guard(mutex)(&it6162->lock);
	it6162->hdcp_sts = HDCP_STATE_IDLE;

	regmap_read(regmap, OFFSET_TX_SETTING, &val);
	val &= 0x0F;
	val |= FIELD_PREP(TX_HDCP_VER_MASK, tx_out->hdcp_version) |
	       FIELD_PREP(TX_HDCP_ENCRYP_MASK, tx_out->hdcp_encryption) |
	       FIELD_PREP(TX_STREAM_TYPE_MASK, tx_out->stream_ID);

	regmap_write(regmap, OFFSET_TX_SETTING, val);
	it6162_infoblock_trigger(it6162, B_SET_CHG);
}

static void it6162_tx_hdcp_disable(struct it6162 *it6162)
{
	int val;
	struct regmap *regmap = it6162->regmap;

	it6162->hdcp_sts = HDCP_STATE_IDLE;
	it6162->hdcp_version = HDCP_DISABLE;

	guard(mutex)(&it6162->lock);
	regmap_read(regmap, OFFSET_TX_SETTING, &val);
	regmap_write(it6162->regmap, OFFSET_TX_SETTING, val & 0x0F);
	it6162_infoblock_trigger(it6162, B_SET_CHG);
}

static void
it6162_tx_hdcp_setup(struct it6162 *it6162, u8 ver, bool encryption)
{
	struct it6162_hdcp_cfg tx_out;

	if (ver != HDCP_DISABLE) {
		tx_out.hdcp_version = ver;
		tx_out.hdcp_encryption = encryption;
		tx_out.stream_ID = 0;
		it6162_tx_hdcp_enable(it6162, &tx_out);
	} else {
		it6162_tx_hdcp_disable(it6162);
	}
}

static void it6162_tx_enable(struct it6162 *it6162)
{
	if (it6162->en_hdcp)
		it6162->hdcp_version = it6162->hdcp_cfg.hdcp_version;
	else
		it6162->hdcp_version = HDCP_DISABLE;
}

static void it6162_tx_disable(struct it6162 *it6162)
{
	cancel_delayed_work_sync(&it6162->hdcp_work);
	it6162_tx_hdcp_setup(it6162, HDCP_DISABLE, false);
}

static inline void
it6162_pack_video_setting(struct it6162 *it6162,
			  struct it6162_video_settings *settings,
			  struct it6162_infoblock_msg *msg)
{
	msg->msg[0x00] = settings->hdew & 0xFF;
	msg->msg[0x01] = (settings->hdew >> 8) & 0x3F;
	msg->msg[0x02] = settings->vdew & 0xFF;
	msg->msg[0x03] = (settings->vdew >> 8) & 0x3F;
	msg->msg[0x04] = settings->clock  & 0xFF;
	msg->msg[0x05] = (settings->clock >> 8) & 0xFF;
	msg->msg[0x06] = (settings->clock >> 16) & 0xFF;
	msg->msg[0x07] = (settings->clock >> 24) & 0xFF;
	msg->msg[0x08] = settings->hfp & 0xFF;
	msg->msg[0x09] = (settings->hfp >> 8) & 0x3F;
	msg->msg[0x0A] = settings->hsw & 0xFF;
	msg->msg[0x0B] = (settings->hsw >> 8) & 0x3F;
	msg->msg[0x0C] = settings->hbp & 0xFF;
	msg->msg[0x0D] = (settings->hbp >> 8) & 0x3F;
	msg->msg[0x0E] = settings->vfp & 0xFF;
	msg->msg[0x0F] = (settings->vfp >> 8) & 0x3F;
	msg->msg[0x10] = settings->vsw & 0xFF;
	msg->msg[0x11] = (settings->vsw >> 8) & 0x3F;
	msg->msg[0x12] = settings->vbp & 0xFF;
	msg->msg[0x13] = (settings->vbp >> 8) & 0x3F;
	msg->msg[0x14] = (settings->prog << 2) |
			 (settings->vpol << 1) |
			  settings->hpol;

	msg->msg[0x15] = settings->v_aspect;
	msg->msg[0x16] = settings->h_aspect & 0xFF;
	msg->msg[0x17] = settings->h_aspect >> 8;
	msg->msg[0x18] = settings->pix_rep;
	msg->msg[0x19] = settings->vic;
	msg->msg[0x1A] = settings->colorspace;
	msg->msg[0x1B] = 1;

	msg->len = 0x1C;
	msg->action = HOST_SET_VIDEO_INFO;
}

static void it6162_mipi_set_video_timing(struct it6162 *it6162,
					 struct it6162_video_settings *mode)
{
	struct it6162_infoblock_msg msg;

	it6162_pack_video_setting(it6162, mode, &msg);
	guard(mutex)(&it6162->lock);
	regmap_bulk_write(it6162->regmap, OFFSET_DATA_BUFFER, &msg.msg,
			  msg.len);
	it6162_infoblock_trigger(it6162, msg.action);
}

static int
it6162_hdcp_read_information(struct it6162 *it6162, u8 *status, u8 *bksv)
{
	u8 buf[DATA_BUFFER_DEPTH];
	int ret;

	guard(mutex)(&it6162->lock);
	ret = it6162_infoblock_request_data(it6162, HOST_SET_HDCP_R, 0x00,
					    buf);
	if (ret < 0)
		return ret;

	memcpy(status, buf, 2);
	memcpy(bksv, buf + 2, 5);
	return 0;
}

static int it6162_hdcp_read_list(struct it6162 *it6162,
				 u8 *ksv_list,
				 int dev_count)
{
	u8 buf[DATA_BUFFER_DEPTH];
	int i, j, ret;

	if (!ksv_list || dev_count <= 0)
		return -EINVAL;

	guard(mutex)(&it6162->lock);
	for (i = 0; i < (dev_count / 6 + 1); i++) {
		ret = it6162_infoblock_request_data(it6162, HOST_SET_HDCP_R,
						    i + 1, buf);
		if (ret < 0)
			return ret;

		for (j = 0; j < 30; j += 5) {
			if ((i * 6 + j / 5) >= dev_count)
				break;

			memcpy(&ksv_list[(i * 6 + j / 5) * 5], &buf[j], 5);
		}
	}
	return dev_count;
}

static void it6162_update_hdcp14(struct it6162 *it6162)
{
	struct drm_device *drm = it6162->drm;
	int dev_count;
	u8 bksv[5];
	u8 bstatus[2];
	u8 ksvlist[5 * 30];
	int ret;

	ret = it6162_hdcp_read_information(it6162, bstatus, bksv);
	if (ret < 0) {
		drm_err(drm, "fail to read Bstatus");
		return;
	}

	dev_count = DRM_HDCP_NUM_DOWNSTREAM(bstatus[0]);
	if (dev_count == 0)
		return;

	if (DRM_HDCP_MAX_DEVICE_EXCEEDED(bstatus[0]) ||
	    DRM_HDCP_MAX_CASCADE_EXCEEDED(bstatus[1])) {
		drm_err(drm, "HDCP1.4 Max Topology Limit Exceeded");
		return;
	}

	dev_count = dev_count > 30 ? 30 : dev_count;
	ret = it6162_hdcp_read_list(it6162, ksvlist, dev_count);
	if (ret < 0) {
		drm_err(drm, "fail to read KSV list");
		return;
	}
}

static void it6162_update_hdcp23(struct it6162 *it6162)
{
	struct drm_device *drm = it6162->drm;
	int dev_count;
	u8 rxid[5];
	u8 rxinfo[2];
	u8 rxid_list[5 * 30];
	int ret;

	ret = it6162_hdcp_read_information(it6162, rxinfo, rxid);
	if (ret < 0) {
		drm_err(drm, "fail to read Rxinfo");
		return;
	}

	dev_count = (HDCP_2_2_DEV_COUNT_HI(rxinfo[0]) << 4 |
		     HDCP_2_2_DEV_COUNT_LO(rxinfo[1]));
	if (dev_count == 0)
		return;

	if (HDCP_2_2_MAX_CASCADE_EXCEEDED(rxinfo[1]) ||
	    HDCP_2_2_MAX_DEVS_EXCEEDED(rxinfo[1])) {
		drm_err(drm, "HDCP2.3 Topology Max Size Exceeded");
		return;
	}

	dev_count = dev_count > 30 ? 30 : dev_count;
	ret = it6162_hdcp_read_list(it6162, rxid_list, dev_count);
	if (ret < 0) {
		drm_err(drm, "fail to read RxID list");
		return;
	}
}

static void it6162_update_hdcp(struct it6162 *it6162, u8 version)
{
	if (version == HDCP_23)
		it6162_update_hdcp23(it6162);
	else
		it6162_update_hdcp14(it6162);
}

static void it6162_hdcp_handler(struct it6162 *it6162)
{
	struct regmap *regmap = it6162->regmap;
	unsigned int tx_status, sink_cap;
	enum hdcp_state hdcp_sts;
	struct it6162_hdcp_cfg *hdcp_cfg = &it6162->hdcp_cfg;
	u8 hdcp_ver;
	u64 cp_status = DRM_MODE_CONTENT_PROTECTION_DESIRED;

	if (hdcp_cfg->hdcp_version == HDCP_DISABLE || !it6162->en_hdcp)
		return;

	if (it6162->hdcp_version != HDCP_DISABLE) {
		regmap_read(regmap, OFFSET_TX_STATUS, &tx_status);
		regmap_read(regmap, OFFSET_SINK_CAP, &sink_cap);
		if (!GET_TX_VIDEO_STATUS(tx_status)) {
			drm_dbg(it6162->drm, "video not stable, skip hdcp check");
			return;
		}

		hdcp_sts = GET_TX_HDCP_STATUS(tx_status);
		hdcp_ver = GET_SINK_CAP_HDCP_VER(sink_cap);
		if (it6162->hdcp_sts != hdcp_sts ||
		    it6162->hdcp_sts == HDCP_STATE_IDLE) {
			it6162->hdcp_sts = hdcp_sts;
			switch (hdcp_sts) {
			case HDCP_STATE_AUTH_DONE:
				it6162_update_hdcp(it6162, hdcp_ver);
				cp_status = DRM_MODE_CONTENT_PROTECTION_ENABLED;
				break;
			case HDCP_STATE_AUTH_FAIL:
				if (hdcp_ver == HDCP_23)
					it6162_tx_hdcp_setup(it6162, HDCP_14,
							     true);
				else
					it6162_tx_hdcp_disable(it6162);

				break;
			default:
				it6162_tx_hdcp_setup(it6162,
						     it6162->hdcp_version,
						     true);
				break;
			}
			drm_hdcp_update_content_protection(it6162->connector,
							   cp_status);
		}
	}
}

static void it6162_interrupt_handler(struct it6162 *it6162)
{
	unsigned int int_status, tx_status;
	enum drm_connector_status connector_status;
	struct device *dev = it6162->dev;
	struct regmap *regmap = it6162->regmap;
	int err;

	err = regmap_read(regmap, OFFSET_EVENT_CHG, &int_status);
	if (err < 0) {
		dev_err(dev, "read failed rx reg[0x%x] err: %d",
			OFFSET_EVENT_CHG, err);
		return;
	}

	regmap_write(it6162->regmap, OFFSET_EVENT_CHG, 0xFF);

	if (!!GET_BUFFER_STATUS(int_status)) {
		regmap_write(it6162->regmap, OFFSET_DRIVER_DATA, int_status);
		it6162->data_buf_sts = GET_BUFFER_STATUS(int_status);
		wake_up(&it6162->data_buf_wait);
	}

	if (!(int_status & EVENT_CHG))
		return;

	err = regmap_read(regmap, OFFSET_TX_STATUS, &tx_status);
	if (err < 0) {
		dev_err(dev, "read failed rx reg[0x%x] err: %d",
			OFFSET_TX_STATUS, err);
		return;
	}

	connector_status = GET_TX_HPD_STATUS(tx_status) ?
			   connector_status_connected :
			   connector_status_disconnected;

	if (it6162->connector_status != connector_status) {
		it6162->connector_status = connector_status;
		drm_bridge_hpd_notify(&it6162->bridge,
				      it6162->connector_status);
	}

	if (it6162->en_hdcp && GET_TX_VIDEO_STATUS(tx_status) &&
	    connector_status == connector_status_connected)
		queue_delayed_work(system_wq, &it6162->hdcp_work,
				   msecs_to_jiffies(2000));
}

static int it6162_wait_ready_event(struct it6162 *it6162)
{
	unsigned int val;
	int ret;
	struct device *dev = it6162->dev;
	struct regmap *regmap = it6162->regmap;

	ret = regmap_read(regmap, OFFSET_EVENT_CHG, &val);
	if (ret < 0) {
		dev_err(dev, "read failed rx reg[0x%x] err: %d",
			OFFSET_EVENT_CHG, ret);
		return ret;
	}

	return val & EVENT_READY;
}

static bool it6162_wait_mcu_ready(struct it6162 *it6162)
{
	int ret, val;

	ret = readx_poll_timeout(it6162_wait_ready_event, it6162, val,
				 val == EVENT_READY, 100 * 1000, 1500 * 1000);
	if (ret) {
		dev_err(it6162->dev, "timeout waiting for MCU ready");
		return false;
	}

	return true;
}

static void it6162_reset_init(struct it6162 *it6162)
{
	struct it6162_mipi_cfg *cfg = &it6162->mipi_cfg;
	u8 cfg_val = 0;

	cfg_val = FIELD_PREP(MIPI_CLOCK_MODE_MASK, cfg->continuous_clk) |
		  FIELD_PREP(MIPI_LANE_SWAP_MASK, cfg->lane_swap) |
		  FIELD_PREP(MIPI_PN_SWAP_MASK, cfg->pn_swap) |
		  FIELD_PREP(MIPI_LANE_NUM_MASK, cfg->lane_num - 1);

	guard(mutex)(&it6162->lock);
	regmap_write(it6162->regmap, OFFSET_MIPI_CONFIG_L, cfg_val);
	regmap_write(it6162->regmap, OFFSET_MIPI_CONFIG_H, cfg->mode);
	regmap_write(it6162->regmap, OFFSET_TX_CONFIG, 0x00);
	regmap_write(it6162->regmap, OFFSET_TX_SETTING, 0x00);
	it6162_infoblock_trigger(it6162, B_CONFIG_CHG | B_SET_CHG);
}

static int it6162_platform_set_power(struct it6162 *it6162)
{
	int err;

	err = regulator_enable(it6162->ivdd);
	if (err)
		return err;

	err = regulator_enable(it6162->pwr1833);
	if (err)
		return err;

	err = regulator_enable(it6162->ovdd);
	if (err)
		return err;

	usleep_range(10000, 20000);
	gpiod_set_value_cansleep(it6162->gpiod_reset, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(it6162->gpiod_reset, 0);
	usleep_range(10000, 20000);

	return 0;
}

static int it6162_platform_clear_power(struct it6162 *it6162)
{
	int err;

	err = regulator_disable(it6162->ivdd);
	if (err)
		return err;

	usleep_range(2000, 3000);

	err = regulator_disable(it6162->pwr1833);
	if (err)
		return err;

	err = regulator_disable(it6162->ovdd);
	if (err)
		return err;

	return 0;
}

static int it6162_detect_devices(struct it6162 *it6162)
{
	struct device *dev = it6162->dev;
	const struct it6162_chip_info *chip_info;
	u32 chip_id, version;
	u8 buf[6];

	if (it6162_platform_set_power(it6162) < 0)
		return -ENODEV;

	/* Firmware must finish booting before register access is valid */
	if (!it6162_wait_mcu_ready(it6162))
		return -ENODEV;

	chip_info = of_device_get_match_data(dev);
	regmap_bulk_read(it6162->regmap, OFFSET_CHIP_ID_L, &buf[0], 6);
	chip_id = (buf[0] << 16) | (buf[1] << 8) | (buf[2]);
	version = (buf[3] << 16) | (buf[4] << 8) | (buf[5]);
	if (chip_id == chip_info->chip_id && version >= chip_info->version)
		return 0;

	dev_err(dev, "chip mismatch: id=0x%06x ver=0x%06x", chip_id, version);
	return -ENODEV;
}

static int __maybe_unused it6162_poweron(struct it6162 *it6162)
{
	int err;

	err = it6162_platform_set_power(it6162);
	if (err < 0)
		return err;

	if (!it6162_wait_mcu_ready(it6162))
		return -ENODEV;

	it6162->connector_status = connector_status_disconnected;
	it6162_reset_init(it6162);
	enable_irq(it6162->it6162_i2c->irq);
	it6162->power_on = true;
	return 0;
}

static int __maybe_unused it6162_poweroff(struct it6162 *it6162)
{
	int err;

	disable_irq(it6162->it6162_i2c->irq);
	err = it6162_platform_clear_power(it6162);
	if (err < 0)
		return err;

	it6162->power_on = false;
	return 0;
}

static void it6162_config_default(struct it6162 *it6162)
{
	struct it6162_mipi_cfg *mipi_cfg = &it6162->mipi_cfg;
	struct it6162_hdcp_cfg *hdcp_cfg = &it6162->hdcp_cfg;

	mipi_cfg->lane_num = 4;
	mipi_cfg->pn_swap = false;
	mipi_cfg->lane_swap = false;
	mipi_cfg->en_port[0] = false;
	mipi_cfg->en_port[1] = false;
	mipi_cfg->continuous_clk = true;
	mipi_cfg->mode = SYNC_EVENT;
	mipi_cfg->format = MIPI_DSI_FMT_RGB888;
	mipi_cfg->mode_flags = MIPI_DSI_MODE_VIDEO;

	it6162->en_hdcp = false;
	hdcp_cfg->hdcp_version = HDCP_AUTO;
	hdcp_cfg->hdcp_encryption = true;
	hdcp_cfg->stream_ID = 0;

	it6162->connector_status = connector_status_disconnected;
}

static enum drm_connector_status it6162_detect(struct it6162 *it6162)
{
	struct regmap *regmap = it6162->regmap;
	unsigned int tx_status;

	regmap_read(regmap, OFFSET_TX_STATUS, &tx_status);
	it6162->connector_status = GET_TX_HPD_STATUS(tx_status) ?
				   connector_status_connected :
				   connector_status_disconnected;
	return it6162->connector_status;
}

static int it6162_get_edid_block(void *data, u8 *buf, unsigned int block,
				 size_t len)
{
	struct it6162 *it6162 = data;
	unsigned int i, cnt;
	int ret;
	u8 config;

	if (len > EDID_LENGTH)
		return -EINVAL;

	guard(mutex)(&it6162->lock);
	for (i = 0, cnt = 0; i < EDID_LENGTH; i += DATA_BUFFER_DEPTH, cnt++) {
		config = (block << 2) | (cnt);
		ret = it6162_infoblock_request_data(it6162, HOST_SET_EDID_R,
						    config, buf + i);
		if (ret < 0)
			return -EIO;
	}

	return 0;
}

static int it6162_audio_update_hw_params(struct it6162 *it6162,
					 struct hdmi_codec_daifmt *fmt,
					 struct hdmi_codec_params *hparms)
{
	enum it6162_audio_select select;
	enum it6162_audio_sample_rate sample_rate;
	unsigned int sample_width;
	unsigned int val;

	switch (hparms->sample_rate) {
	case 32000:
		sample_rate = SAMPLE_RATE_32K;
		break;
	case 44100:
		sample_rate = SAMPLE_RATE_44_1K;
		break;
	case 48000:
		sample_rate = SAMPLE_RATE_48K;
		break;
	case 88200:
		sample_rate = SAMPLE_RATE_88_2K;
		break;
	case 96000:
		sample_rate = SAMPLE_RATE_96K;
		break;
	case 176400:
		sample_rate = SAMPLE_RATE_176_4K;
		break;
	case 192000:
		sample_rate = SAMPLE_RATE_192K;
		break;
	default:
		return -EINVAL;
	}

	switch (hparms->sample_width) {
	case 16:
		sample_width = WORD_LENGTH_16BIT;
		break;
	case 18:
		sample_width = WORD_LENGTH_18BIT;
		break;
	case 20:
		sample_width = WORD_LENGTH_20BIT;
		break;
	case 24:
		sample_width = WORD_LENGTH_24BIT;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt->fmt) {
	case HDMI_I2S:
		select = I2S;
		break;
	case HDMI_SPDIF:
		select = SPDIF;
		break;
	default:
		return -EINVAL;
	}

	guard(mutex)(&it6162->lock);
	val = FIELD_PREP(MASK_AUDIO_CHANNEL_NUM, hparms->channels) |
	      FIELD_PREP(MASK_AUDIO_SELECT, select) |
	      FIELD_PREP(MASK_AUDIO_TYPE, LPCM);
	regmap_write(it6162->regmap, OFFSET_AUDIO_CTRL0, val);
	regmap_write(it6162->regmap, OFFSET_AUDIO_CTRL1, sample_rate);

	val = FIELD_PREP(MASK_I2S_BUS_MODE, 0) |
	      FIELD_PREP(MASK_I2S_WORD_LEN, sample_width) |
	      FIELD_PREP(MASK_I2S_VALID, 1);
	regmap_write(it6162->regmap, OFFSET_AUDIO_CTRL2, val);
	return it6162_infoblock_trigger(it6162, HOST_SET_AUDIO_INFO);
}

static void it6162_disable_audio(struct it6162 *it6162)
{
	guard(mutex)(&it6162->lock);
	regmap_write(it6162->regmap, OFFSET_AUDIO_CTRL0, 0x00);
	regmap_write(it6162->regmap, OFFSET_AUDIO_CTRL1, 0x00);
	regmap_write(it6162->regmap, OFFSET_AUDIO_CTRL2, 0x00);
	it6162_infoblock_trigger(it6162, HOST_SET_AUDIO_INFO);
}

static irqreturn_t it6162_int_threaded_handler(int unused, void *data)
{
	struct it6162 *it6162 = data;

	it6162_interrupt_handler(it6162);
	return IRQ_HANDLED;
}

static void it6162_hdcp_work(struct work_struct *work)
{
	struct it6162 *it6162 = container_of(work, struct it6162,
					     hdcp_work.work);

	it6162_hdcp_handler(it6162);
}

static struct mipi_dsi_host *
it6162_of_get_dsi_host_by_port(struct it6162 *it6162, int port)
{
	struct device_node *of = it6162->dev->of_node;
	struct device_node *host_node;
	struct device_node *endpoint;
	struct mipi_dsi_host *dsi_host;

	endpoint = of_graph_get_endpoint_by_regs(of, port, -1);
	if (!endpoint)
		return ERR_PTR(-ENODEV);

	host_node = of_graph_get_remote_port_parent(endpoint);
	of_node_put(endpoint);
	if (!host_node)
		return ERR_PTR(-ENODEV);

	dsi_host = of_find_mipi_dsi_host_by_node(host_node);
	of_node_put(host_node);
	if (!dsi_host)
		return ERR_PTR(-EPROBE_DEFER);

	return dsi_host;
}

static int it6162_of_get_dsi_host(struct it6162 *it6162)
{
	struct mipi_dsi_host *dsi_host;
	int port, host_count = 0;

	for (port = 0; port < 2; port++) {
		dsi_host = it6162_of_get_dsi_host_by_port(it6162, port);
		if (IS_ERR(dsi_host)) {
			if (PTR_ERR(dsi_host) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			continue;
		}

		host_count++;
	}

	return host_count == 0 ? -ENODEV : 0;
}

static int it6162_load_mipi_pars_from_port(struct it6162 *it6162, int port)
{
	struct device_node *of = it6162->dev->of_node;
	struct device_node *endpoint;
	struct it6162_mipi_cfg *mipicfg = &it6162->mipi_cfg;
	u32 data_lanes[4] = {1};
	u32 lane_polarities[5] = {0};
	int dsi_lanes;

	endpoint = of_graph_get_endpoint_by_regs(of, port, -1);
	if (!endpoint)
		return 0;

	dsi_lanes = drm_of_get_data_lanes_count(endpoint, 1, 4);
	if (dsi_lanes < 0)
		return dsi_lanes;

	mipicfg->lane_num = dsi_lanes;

	of_property_read_u32_array(endpoint, "data-lanes", data_lanes,
				   dsi_lanes);
	/* only supports full reversal of all lanes, not arbitrary remapping */
	if (data_lanes[0] == 3)
		mipicfg->lane_swap  = true;

	of_property_read_u32_array(endpoint, "lane-polarities",
				   lane_polarities, dsi_lanes + 1);
	/* applies Dp/Dn swap globally; mixing polarities across lanes is invalid */
	if (lane_polarities[0] != 0)
		mipicfg->pn_swap = true;

	if (of_property_present(endpoint, "clock-noncontinuous")) {
		mipicfg->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;
		mipicfg->continuous_clk = false;
	}

	of_node_put(endpoint);
	return dsi_lanes;
}

static int it6162_attach_dsi(struct it6162 *it6162)
{
	struct device *dev = it6162->dev;
	struct device_node *np = dev->of_node;
	const struct mipi_dsi_device_info info = {"it6162-mipi", 0, np};
	struct mipi_dsi_device *dsi;
	struct mipi_dsi_host *host;
	struct it6162_mipi_cfg *mipi_cfg = &it6162->mipi_cfg;
	int ret = 0;

	it6162->dsi = NULL;
	for (int port = 0; port < 2; port++) {
		host = it6162_of_get_dsi_host_by_port(it6162, port);
		if (IS_ERR(host))
			continue;

		mipi_cfg->en_port[port] = true;

		dsi = devm_mipi_dsi_device_register_full(dev, host, &info);
		if (IS_ERR(dsi)) {
			dev_err(dev, "failed to create dsi device");
			return PTR_ERR(dsi);
		}
		/* uses a single DSI configuration for both ports in dual-link mode */
		if (!it6162->dsi) {
			ret = it6162_load_mipi_pars_from_port(it6162, port);
			if (ret <= 0)
				return ret;
			it6162->dsi = dsi;
		}

		dsi->lanes = mipi_cfg->lane_num;
		dsi->format = mipi_cfg->format;
		dsi->mode_flags = mipi_cfg->mode_flags;
		ret = devm_mipi_dsi_attach(dev, dsi);
		if (ret) {
			dev_err(dev, "failed to attach dsi device %d", port);
			return ret;
		}
	}

	it6162_poweron(it6162);
	return 0;
}

static bool it6162_of_get_audio(struct it6162 *it6162)
{
	struct device_node *np = it6162->dev->of_node;
	struct device_node *audio_port;

	audio_port = of_graph_get_port_by_id(np, 2);
	if (audio_port) {
		of_node_put(audio_port);
		return true;
	}

	return false;
}

static int it6162_init_pdata(struct it6162 *it6162)
{
	struct device *dev = it6162->dev;

	it6162->ivdd = devm_regulator_get(dev, "ivdd");
	if (IS_ERR(it6162->ivdd))
		return dev_err_probe(dev, PTR_ERR(it6162->ivdd),
				     "failed to get ivdd regulator");

	it6162->pwr1833 = devm_regulator_get(dev, "ovdd1833");
	if (IS_ERR(it6162->pwr1833))
		return dev_err_probe(dev, PTR_ERR(it6162->pwr1833),
				     "failed to get ovdd1833 regulator");

	it6162->ovdd = devm_regulator_get(dev, "ovdd");
	if (IS_ERR(it6162->ovdd))
		return dev_err_probe(dev, PTR_ERR(it6162->ovdd),
				     "failed to get ovdd regulator");

	it6162->gpiod_reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(it6162->gpiod_reset))
		return dev_err_probe(dev, PTR_ERR(it6162->gpiod_reset),
				     "failed to get reset gpio\n");

	return 0;
}

static int it6162_bridge_attach(struct drm_bridge *bridge,
				struct drm_encoder *encoder,
				enum drm_bridge_attach_flags flags)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);
	struct drm_device *drm = bridge->dev;

	it6162->drm = drm;
	if (!drm_core_check_feature(drm, DRIVER_ATOMIC)) {
		drm_dbg(drm, "it6162 driver only copes with atomic updates");
		return -EOPNOTSUPP;
	}

	return 0;
}

static enum drm_mode_status
it6162_bridge_mode_valid(struct drm_bridge *bridge,
			 const struct drm_display_info *info,
			 const struct drm_display_mode *mode)
{
	unsigned long long rate;

	rate = drm_hdmi_compute_mode_clock(mode, 8, HDMI_COLORSPACE_RGB);
	if (rate == 0)
		return MODE_NOCLOCK;

	return bridge->funcs->hdmi_tmds_char_rate_valid(bridge, mode, rate);
}

static enum drm_connector_status
it6162_bridge_detect(struct drm_bridge *bridge,
		     struct drm_connector *connector)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return it6162->power_on ? it6162_detect(it6162) :
				connector_status_disconnected;
}

static inline void
it6162_avi_to_video_setting(struct hdmi_avi_infoframe *avi_info,
			    struct it6162_video_settings *video)
{
	video->vic = avi_info->video_code;

	switch (avi_info->picture_aspect) {
	case HDMI_PICTURE_ASPECT_4_3:
		video->h_aspect = 4;
		video->v_aspect = 3;
		break;
	case HDMI_PICTURE_ASPECT_16_9:
		video->h_aspect = 16;
		video->v_aspect = 9;
		break;
	case HDMI_PICTURE_ASPECT_64_27:
		video->h_aspect = 64;
		video->v_aspect = 27;
		break;
	case HDMI_PICTURE_ASPECT_256_135:
		video->h_aspect = 256;
		video->v_aspect = 135;
		break;
	default:
		video->h_aspect = 4;
		video->v_aspect = 3;
		break;
	}

	video->pix_rep = avi_info->pixel_repeat + 1;
}

static inline void
it6162_display_mode_to_settings(struct drm_display_mode *mode,
				struct it6162_video_settings *settings)
{
	if (mode->flags & DRM_MODE_FLAG_PHSYNC)
		settings->hpol = 1;

	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		settings->vpol = 1;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		settings->prog = 0;

	settings->clock = mode->clock;
	settings->hdew = mode->hdisplay;
	settings->hfp = mode->hsync_start - mode->hdisplay;
	settings->hsw = mode->hsync_end - mode->hsync_start;
	settings->hbp = mode->htotal - mode->hsync_end;
	settings->htotal = mode->htotal;
	settings->vdew = mode->vdisplay;
	settings->vfp = mode->vsync_start - mode->vdisplay;
	settings->vsw = mode->vsync_end - mode->vsync_start;
	settings->vbp = mode->vtotal - mode->vsync_end;
	settings->vtotal = mode->vtotal;
}

static void it6162_bridge_atomic_enable(struct drm_bridge *bridge,
					struct drm_atomic_state *state)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	struct drm_connector *connector;
	struct hdmi_avi_infoframe *avi;
	struct it6162_video_settings video_setting;

	connector = drm_atomic_get_new_connector_for_encoder(state,
							     bridge->encoder);
	if (!connector)
		return;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (!conn_state)
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (!crtc_state)
		return;

	it6162->connector = connector;
	it6162->content_protection = conn_state->content_protection;

	drm_atomic_helper_connector_hdmi_update_infoframes(connector, state);
	avi = &conn_state->hdmi.infoframes.avi.data.avi;
	video_setting.colorspace = conn_state->hdmi.output_format;
	it6162_avi_to_video_setting(avi, &video_setting);
	it6162_display_mode_to_settings(&crtc_state->mode, &video_setting);

	it6162_mipi_set_video_timing(it6162, &video_setting);
	it6162_tx_enable(it6162);
	it6162_mipi_enable(it6162);
}

static int it6162_bridge_atomic_check(struct drm_bridge *bridge,
				      struct drm_bridge_state *bridge_state,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state)
{
	struct drm_display_mode *adj = &crtc_state->adjusted_mode;
	struct drm_display_mode *mode = &crtc_state->mode;
	struct it6162 *it6162 = bridge_to_it6162(bridge);
	u32 hfp, hsw, hbp;
	u32 clock;
	u32 hfp_check;

	clock = mode->clock;
	hfp = mode->hsync_start - mode->hdisplay;
	hsw = mode->hsync_end - mode->hsync_start;
	hbp = mode->htotal - mode->hsync_end;

	hfp_check = DIV_ROUND_UP(65 * clock, 1000000) + 4;
	if (hfp >= hfp_check)
		return 0;

	if (hbp > hfp_check - hfp) {
		adj->hsync_start = adj->hdisplay + hfp_check;
		adj->hsync_end = adj->hsync_start + hsw;
		drm_dbg(it6162->drm, "adjusted hsync on MIPI bus: %u, %u",
			adj->hsync_start, adj->hsync_end);
	}

	return 0;
}

static void it6162_bridge_atomic_disable(struct drm_bridge *bridge,
					 struct drm_atomic_state *state)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	it6162_tx_disable(it6162);
	it6162_mipi_disable(it6162);
}

static const struct drm_edid *
it6162_bridge_read_edid(struct drm_bridge *bridge,
			struct drm_connector *connector)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return drm_edid_read_custom(connector, it6162_get_edid_block, it6162);
}

static int it6162_bridge_hdmi_audio_prepare(struct drm_bridge *bridge,
					    struct drm_connector *connector,
					    struct hdmi_codec_daifmt *fmt,
					    struct hdmi_codec_params *params)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);
	int ret;

	ret = it6162_audio_update_hw_params(it6162, fmt, params);
	if (ret < 0)
		return ret;

	return drm_atomic_helper_connector_hdmi_update_audio_infoframe(connector,
								       &params->cea);
}

static void it6162_bridge_hdmi_audio_shutdown(struct drm_bridge *bridge,
					      struct drm_connector *connector)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	drm_atomic_helper_connector_hdmi_clear_audio_infoframe(connector);
	it6162_disable_audio(it6162);
}

static enum drm_mode_status
it6162_hdmi_tmds_char_rate_valid(const struct drm_bridge *bridge,
				 const struct drm_display_mode *mode,
				 unsigned long long tmds_rate)
{
	/*IT6162 hdmi supports HDMI2.0 600Mhz*/
	if (tmds_rate > 600000000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static inline int
it6162_write_infoframe(struct it6162 *it6162, const u8 *buffer, size_t len)
{
	if (len > DATA_BUFFER_DEPTH)
		return -EINVAL;

	regmap_bulk_write(it6162->regmap, OFFSET_DATA_BUFFER, buffer, len);
	regmap_write(it6162->regmap, OFFSET_DATA_TYPE_IDX, len);
	it6162_infoblock_trigger(it6162, HOST_SET_CEA_INFOFRAME);
	return 0;
}

static inline int it6162_clear_infoframe(struct it6162 *it6162, u8 type)
{
	regmap_write(it6162->regmap, OFFSET_DATA_TYPE_IDX, 3);
	regmap_write(it6162->regmap, OFFSET_DATA_BUFFER, type);
	regmap_write(it6162->regmap, OFFSET_DATA_BUFFER + 1, 0x00);
	regmap_write(it6162->regmap, OFFSET_DATA_BUFFER + 2, 0x00);
	it6162_infoblock_trigger(it6162, HOST_SET_CEA_INFOFRAME);
	return 0;
}

static int
it6162_bridge_hdmi_write_avi_infoframe(struct drm_bridge *bridge,
				       const u8 *buffer, size_t len)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return it6162_write_infoframe(it6162, buffer, len);
}

static int
it6162_bridge_hdmi_clear_avi_infoframe(struct drm_bridge *bridge)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return it6162_clear_infoframe(it6162, HDMI_INFOFRAME_TYPE_AVI);
}

static int
it6162_bridge_hdmi_write_audio_infoframe(struct drm_bridge *bridge,
					 const u8 *buffer, size_t len)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return it6162_write_infoframe(it6162, buffer, len);
}

static int
it6162_bridge_hdmi_clear_audio_infoframe(struct drm_bridge *bridge)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return it6162_clear_infoframe(it6162, HDMI_INFOFRAME_TYPE_AUDIO);
}

static int
it6162_bridge_hdmi_write_spd_infoframe(struct drm_bridge *bridge,
				       const u8 *buffer, size_t len)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return it6162_write_infoframe(it6162, buffer, len);
}

static int
it6162_bridge_hdmi_clear_spd_infoframe(struct drm_bridge *bridge)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return it6162_clear_infoframe(it6162, HDMI_INFOFRAME_TYPE_SPD);
}

static int
it6162_bridge_hdmi_write_hdmi_infoframe(struct drm_bridge *bridge,
					const u8 *buffer, size_t len)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return it6162_write_infoframe(it6162, buffer, len);
}

static int
it6162_bridge_hdmi_clear_hdmi_infoframe(struct drm_bridge *bridge)
{
	struct it6162 *it6162 = bridge_to_it6162(bridge);

	return it6162_clear_infoframe(it6162, HDMI_INFOFRAME_TYPE_VENDOR);
}

static const struct drm_bridge_funcs it6162_bridge_funcs = {
	.attach = it6162_bridge_attach,
	.mode_valid = it6162_bridge_mode_valid,
	.detect = it6162_bridge_detect,

	.atomic_enable = it6162_bridge_atomic_enable,
	.atomic_disable = it6162_bridge_atomic_disable,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_check = it6162_bridge_atomic_check,

	.edid_read = it6162_bridge_read_edid,

	.hdmi_clear_hdmi_infoframe = it6162_bridge_hdmi_clear_hdmi_infoframe,
	.hdmi_write_hdmi_infoframe = it6162_bridge_hdmi_write_hdmi_infoframe,
	.hdmi_clear_avi_infoframe = it6162_bridge_hdmi_clear_avi_infoframe,
	.hdmi_write_avi_infoframe = it6162_bridge_hdmi_write_avi_infoframe,
	.hdmi_clear_spd_infoframe = it6162_bridge_hdmi_clear_spd_infoframe,
	.hdmi_write_spd_infoframe = it6162_bridge_hdmi_write_spd_infoframe,
	.hdmi_clear_audio_infoframe = it6162_bridge_hdmi_clear_audio_infoframe,
	.hdmi_write_audio_infoframe = it6162_bridge_hdmi_write_audio_infoframe,

	.hdmi_tmds_char_rate_valid = it6162_hdmi_tmds_char_rate_valid,
	.hdmi_audio_prepare = it6162_bridge_hdmi_audio_prepare,
	.hdmi_audio_shutdown = it6162_bridge_hdmi_audio_shutdown,
};

static int it6162_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	struct it6162 *it6162;
	int ret;

	it6162 = devm_drm_bridge_alloc(dev, struct it6162, bridge,
				       &it6162_bridge_funcs);
	if (IS_ERR(it6162))
		return PTR_ERR(it6162);

	it6162->dev = dev;

	ret = it6162_of_get_dsi_host(it6162);
	if (ret < 0)
		return ret;

	ret = it6162_i2c_regmap_init(client, it6162);
	if (ret != 0)
		return ret;

	ret = it6162_init_pdata(it6162);
	if (ret)
		return ret;

	it6162_config_default(it6162);

	if (it6162_detect_devices(it6162) < 0)
		return -ENODEV;

	if (!client->irq) {
		dev_err(dev, "Failed to get INTP IRQ");
		return -ENODEV;
	}

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					it6162_int_threaded_handler,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT |
					IRQF_NO_AUTOEN,
					"it6162-intp", it6162);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&it6162->hdcp_work, it6162_hdcp_work);
	init_waitqueue_head(&it6162->data_buf_wait);

	mutex_init(&it6162->lock);

	it6162->bridge.of_node = np;
	it6162->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID |
			     DRM_BRIDGE_OP_HDMI | DRM_BRIDGE_OP_MODES;

	it6162->bridge.type = DRM_MODE_CONNECTOR_HDMIA;

	it6162->bridge.vendor = "ITE";
	it6162->bridge.product = "IT6162";

	if (it6162_of_get_audio(it6162)) {
		it6162->bridge.ops |= DRM_BRIDGE_OP_HDMI_AUDIO;
		it6162->bridge.hdmi_audio_dev = dev;
		it6162->bridge.hdmi_audio_max_i2s_playback_channels = 8;
		it6162->bridge.hdmi_audio_dai_port = 2;
	}

	devm_drm_bridge_add(dev, &it6162->bridge);

	return it6162_attach_dsi(it6162);
}

static void it6162_remove(struct i2c_client *client)
{
	struct it6162 *it6162 = i2c_get_clientdata(client);

	disable_irq(client->irq);
	cancel_delayed_work_sync(&it6162->hdcp_work);
	mutex_destroy(&it6162->lock);
}

static const struct it6162_chip_info it6162_chip_info = {
	.chip_id = 0x616200,
	.version = 0x006500,
};

static const struct of_device_id it6162_dt_ids[] = {
	{ .compatible = "ite,it6162", .data = &it6162_chip_info},
	{ }
};
MODULE_DEVICE_TABLE(of, it6162_dt_ids);

static const struct i2c_device_id it6162_i2c_ids[] = {
	{ "it6162", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, it6162_i2c_ids);

static struct i2c_driver it6162_driver = {
	.driver = {
		.name = "it6162",
		.of_match_table = it6162_dt_ids,
	},
	.probe = it6162_probe,
	.remove = it6162_remove,
	.id_table = it6162_i2c_ids,
};
module_i2c_driver(it6162_driver);

MODULE_AUTHOR("Pet Weng <pet.weng@ite.com.tw>");
MODULE_AUTHOR("Hermes Wu <Hermes.Wu@ite.com.tw>");
MODULE_DESCRIPTION("it6162 MIPI to HDMI driver");
MODULE_LICENSE("GPL");
