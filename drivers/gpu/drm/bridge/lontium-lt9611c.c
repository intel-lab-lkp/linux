// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Lontium Semiconductor, Inc.
 */

#include <linux/crc8.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include <drm/display/drm_hdmi_audio_helper.h>
#include <drm/display/drm_hdmi_state_helper.h>
#include <sound/hdmi-codec.h>

#define FW_SIZE (64 * 1024)
#define LT_PAGE_SIZE 256
#define FW_FILE  "LT9611C.bin"
#define LT9611C_CRC_POLYNOMIAL 0x31
#define LT9611C_PAGE_CONTROL 0xff

struct lt9611c {
	struct device *dev;
	struct i2c_client *client;
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;
	struct regmap *regmap;
	/* Protects all accesses to registers by stopping the on-chip MCU */
	struct mutex ocm_lock;
	struct work_struct work;
	struct device_node *dsi0_node;
	struct device_node *dsi1_node;
	struct mipi_dsi_device *dsi0;
	struct mipi_dsi_device *dsi1;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[2];
	u32 chip_type;
	const struct firmware *fw;
	int fw_version;
	u8 fw_crc;
	bool hdmi_connected;
};

DECLARE_CRC8_TABLE(lt9611c_crc8_table);

static const struct regmap_range_cfg lt9611c_ranges[] = {
	{
		.name = "register_range",
		.range_min =  0,
		.range_max = 0xffff,
		.selector_reg = LT9611C_PAGE_CONTROL,
		.selector_mask = 0xff,
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 0x100,
	},
};

static const struct regmap_config lt9611c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xffff,
	.ranges = lt9611c_ranges,
	.num_ranges = ARRAY_SIZE(lt9611c_ranges),
};

static int lt9611c_read_write_flow(struct lt9611c *lt9611c, u8 *params,
				   unsigned int param_count, u8 *return_buffer,
				   unsigned int return_count)
{
	int count, i;
	unsigned int temp;

	regmap_write(lt9611c->regmap, 0xe0de, 0x01);

	count = 0;
	do {
		regmap_read(lt9611c->regmap, 0xe0ae, &temp);
		usleep_range(1000, 2000);
		count++;
	} while (count < 100 && temp != 0x01);

	if (temp != 0x01)
		return -1;

	for (i = 0; i < param_count; i++) {
		if (i > 0xdd - 0xb0)
			break;

		regmap_write(lt9611c->regmap, 0xe0b0 + i, params[i]);
	}

	regmap_write(lt9611c->regmap, 0xe0de, 0x02);

	count = 0;
	do {
		regmap_read(lt9611c->regmap, 0xe0ae, &temp);
		usleep_range(1000, 2000);
		count++;
	} while (count < 100 && temp != 0x02);

	if (temp != 0x02)
		return -2;

	regmap_bulk_read(lt9611c->regmap, 0xe085, return_buffer, return_count);

	return 0;
}

static int lt9611c_prepare_firmware_data(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int ret;
	u8 *buffer;
	size_t total_size = FW_SIZE - 1;

	ret = request_firmware(&lt9611c->fw, FW_FILE, dev);
	if (ret) {
		dev_err(dev, "failed load file '%s', error type %d\n", FW_FILE, ret);
		return -EPROBE_DEFER;
	}

	if (lt9611c->fw->size > total_size) {
		dev_err(dev, "firmware too large (%zu > %zu)\n", lt9611c->fw->size, total_size);
		release_firmware(lt9611c->fw);
		lt9611c->fw = NULL;
		return -EINVAL;
	}

	dev_dbg(dev, "firmware size: %zu bytes\n", lt9611c->fw->size);

	buffer = kzalloc(total_size, GFP_KERNEL);
	if (!buffer) {
		release_firmware(lt9611c->fw);
		lt9611c->fw = NULL;
		return -ENOMEM;
	}

	memset(buffer, 0xff, total_size);
	memcpy(buffer, lt9611c->fw->data, lt9611c->fw->size);

	lt9611c->fw_crc = crc8(lt9611c_crc8_table, buffer, total_size, 0);

	dev_dbg(dev, "firmware crc: 0x%02x\n", lt9611c->fw_crc);

	kfree(buffer);
	return 0;
}

static void lt9611c_config_parameters(struct lt9611c *lt9611c)
{
	const struct reg_sequence seq_write_paras[] = {
		REG_SEQ0(0xe0ee, 0x01),
		REG_SEQ0(0xe103, 0x3f), //fifo rst
		REG_SEQ0(0xe103, 0xff),
		REG_SEQ0(0xe05e, 0xc1),
		REG_SEQ0(0xe058, 0x00),
		REG_SEQ0(0xe059, 0x50),
		REG_SEQ0(0xe05a, 0x10),
		REG_SEQ0(0xe05a, 0x00),
		REG_SEQ0(0xe058, 0x21),
	};

	regmap_multi_reg_write(lt9611c->regmap, seq_write_paras, ARRAY_SIZE(seq_write_paras));
}

static void lt9611c_wren(struct lt9611c *lt9611c)
{
	regmap_write(lt9611c->regmap, 0xe05a, 0x04);
	regmap_write(lt9611c->regmap, 0xe05a, 0x00);
}

static void lt9611c_wrdi(struct lt9611c *lt9611c)
{
	regmap_write(lt9611c->regmap, 0xe05a, 0x08);
	regmap_write(lt9611c->regmap, 0xe05a, 0x00);
}

static void lt9611c_erase_op(struct lt9611c *lt9611c, u32 addr)
{
	const struct reg_sequence seq_write[] = {
		REG_SEQ0(0xe0ee, 0x01),
		REG_SEQ0(0xe05a, 0x04),
		REG_SEQ0(0xe05a, 0x00),
		REG_SEQ0(0xe05b, (addr >> 16) & 0xff),
		REG_SEQ0(0xe05c, (addr >> 8) & 0xff),
		REG_SEQ0(0xe05d, addr & 0xff),
		REG_SEQ0(0xe05a, 0x01),
		REG_SEQ0(0xe05a, 0x00),
	};

	regmap_multi_reg_write(lt9611c->regmap, seq_write, ARRAY_SIZE(seq_write));
}

static void read_flash_reg_status(struct lt9611c *lt9611c, unsigned int *status)
{
	const struct reg_sequence seq_write[] = {
		REG_SEQ0(0xe103, 0x3f),
		REG_SEQ0(0xe103, 0xff),
		REG_SEQ0(0xe05e, 0x40),
		REG_SEQ0(0xe056, 0x05),
		REG_SEQ0(0xe055, 0x25),
		REG_SEQ0(0xe055, 0x01),
		REG_SEQ0(0xe058, 0x21),
	};

	regmap_multi_reg_write(lt9611c->regmap, seq_write, ARRAY_SIZE(seq_write));

	regmap_read(lt9611c->regmap, 0xe05f, status);
}

static void lt9611c_crc_to_sram(struct lt9611c *lt9611c)
{
	const struct reg_sequence seq_write[] = {
		REG_SEQ0(0xe051, 0x00),
		REG_SEQ0(0xe055, 0xc0),
		REG_SEQ0(0xe055, 0x80),
		REG_SEQ0(0xe05e, 0xc0),
		REG_SEQ0(0xe058, 0x21),
	};

	regmap_multi_reg_write(lt9611c->regmap, seq_write, ARRAY_SIZE(seq_write));
}

static void lt9611c_data_to_sram(struct lt9611c *lt9611c)
{
	const struct reg_sequence seq_write[] = {
		REG_SEQ0(0xe051, 0xff),
		REG_SEQ0(0xe055, 0x80),
		REG_SEQ0(0xe05e, 0xc0),
		REG_SEQ0(0xe058, 0x21),
	};

	regmap_multi_reg_write(lt9611c->regmap, seq_write, ARRAY_SIZE(seq_write));
}

static void lt9611c_sram_to_flash(struct lt9611c *lt9611c, size_t addr)
{
	const struct reg_sequence seq_write[] = {
		REG_SEQ0(0xe05b, (addr >> 16) & 0xff),
		REG_SEQ0(0xe05c, (addr >> 8) & 0xff),
		REG_SEQ0(0xe05d, addr & 0xff),
		REG_SEQ0(0xe05a, 0x30),
		REG_SEQ0(0xe05a, 0x00),
	};

	regmap_multi_reg_write(lt9611c->regmap, seq_write, ARRAY_SIZE(seq_write));
}

static void lt9611c_block_erase(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int i;
	unsigned int block_num;
	unsigned int flash_status = 0;
	u32 flash_addr = 0;

	for (block_num = 0; block_num < 2; block_num++) {
		flash_addr = (block_num * 0x008000);
		lt9611c_erase_op(lt9611c, flash_addr);
		msleep(100);
		i = 0;
		while (1) {
			read_flash_reg_status(lt9611c, &flash_status);
			if ((flash_status & 0x01) == 0)
				break;

			if (i > 50)
				break;

			i++;
			msleep(50);
		}
	}

	dev_dbg(dev, "erase flash done.\n");
}

static int lt9611c_write_data(struct lt9611c *lt9611c, size_t addr)
{
	struct device *dev = lt9611c->dev;
	int ret;
	unsigned int page = 0, num = 0, i = 0;
	size_t size, index;
	const u8 *data;
	u8 value;

	data = lt9611c->fw->data;
	size = lt9611c->fw->size;
	page = (size + LT_PAGE_SIZE - 1) / LT_PAGE_SIZE;
	if (page * LT_PAGE_SIZE > FW_SIZE) {
		dev_err(dev, "firmware size out of range\n");
		return -EINVAL;
	}

	dev_dbg(dev, "%u pages, total size %zu byte\n", page, size);

	for (num = 0; num < page; num++) {
		lt9611c_data_to_sram(lt9611c);

		for (i = 0; i < LT_PAGE_SIZE; i++) {
			index = num * LT_PAGE_SIZE + i;
			value = (index < size) ? data[index] : 0xff;

			ret = regmap_write(lt9611c->regmap, 0xe059, value);
			if (ret < 0) {
				dev_err(dev, "write error at page %u, index %u\n", num, i);
				return ret;
			}
		}

		lt9611c_wren(lt9611c);
		lt9611c_sram_to_flash(lt9611c, addr);

		addr += LT_PAGE_SIZE;
	}

	lt9611c_wrdi(lt9611c);

	return 0;
}

static int lt9611c_write_crc(struct lt9611c *lt9611c, size_t addr)
{
	struct device *dev = lt9611c->dev;
	int ret;
	u8 crc;

	crc = lt9611c->fw_crc;
	lt9611c_crc_to_sram(lt9611c);
	ret = regmap_write(lt9611c->regmap, 0xe059, crc);
	if (ret < 0) {
		dev_err(dev, "failed to write crc\n");
		return ret;
	}

	lt9611c_wren(lt9611c);
	lt9611c_sram_to_flash(lt9611c, addr);
	lt9611c_wrdi(lt9611c);

	dev_dbg(dev, "crc 0x%02x written to flash at addr 0x%zx\n", crc, addr);

	return 0;
}

static int lt9611c_firmware_upgrade(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int ret;

	ret = lt9611c_prepare_firmware_data(lt9611c);
	if (ret < 0)
		return ret;

	dev_dbg(dev, "starting firmware upgrade, size: %zu bytes\n", lt9611c->fw->size);

	lt9611c_config_parameters(lt9611c);
	lt9611c_block_erase(lt9611c);

	ret = lt9611c_write_data(lt9611c, 0);
	if (ret < 0) {
		dev_err(dev, "failed to write firmware data\n");
		return ret;
	}

	release_firmware(lt9611c->fw);
	lt9611c->fw = NULL;

	ret = lt9611c_write_crc(lt9611c, FW_SIZE - 1);
	if (ret < 0) {
		dev_err(dev, "failed to write firmware crc\n");
		return ret;
	}

	return 0;
}

static int lt9611c_upgrade_result(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	unsigned int crc_result;

	regmap_write(lt9611c->regmap, 0xe0ee, 0x01);
	regmap_read(lt9611c->regmap, 0xe021, &crc_result);

	if (crc_result != lt9611c->fw_crc) {
		dev_err(dev, "lt9611c fw upgrade failed, expected crc=0x%02x, read crc=0x%02x\n",
			lt9611c->fw_crc, crc_result);
		return -1;
	}

	dev_dbg(dev, "lt9611c firmware upgrade success, crc=0x%02x\n", crc_result);
	return 0;
}

static struct lt9611c *bridge_to_lt9611c(struct drm_bridge *bridge)
{
	return container_of(bridge, struct lt9611c, bridge);
}

/*read only*/
static const struct lt9611c *bridge_to_lt9611c_const(const struct drm_bridge *bridge)
{
	return container_of(bridge, const struct lt9611c, bridge);
}

static void lt9611c_lock(struct lt9611c *lt9611c)
{
	mutex_lock(&lt9611c->ocm_lock);
	regmap_write(lt9611c->regmap, 0xe0ee, 0x01);
}

static void lt9611c_unlock(struct lt9611c *lt9611c)
{
	regmap_write(lt9611c->regmap, 0xe0ee, 0x00);
	mutex_unlock(&lt9611c->ocm_lock);
}

static irqreturn_t lt9611c_irq_thread_handler(int irq, void *dev_id)
{
	struct lt9611c *lt9611c = dev_id;
	struct device *dev = lt9611c->dev;
	int ret;
	unsigned int irq_status;
	u8 cmd[5] = {0x52, 0x48, 0x31, 0x3a, 0x00};
	u8 data[5];

	mutex_lock(&lt9611c->ocm_lock);

	regmap_read(lt9611c->regmap, 0xe084, &irq_status);
	if (!(irq_status & BIT(0))) {
		mutex_unlock(&lt9611c->ocm_lock);
		return IRQ_HANDLED;
	}

	ret = lt9611c_read_write_flow(lt9611c, cmd, 5, data, 5);
	if (ret) {
		dev_err(dev, "failed to read HPD status\n");
	} else {
		lt9611c->hdmi_connected = (data[4] == 0x02);
		dev_dbg(dev, "HDMI %s\n", lt9611c->hdmi_connected ? "connected" : "disconnected");
	}

	schedule_work(&lt9611c->work);

	/*clear interrupt*/
	regmap_write(lt9611c->regmap, 0xe0df, irq_status & BIT(0));
	//hardware need delay
	usleep_range(10000, 12000);
	regmap_write(lt9611c->regmap, 0xe0df, irq_status & (~BIT(0)));

	mutex_unlock(&lt9611c->ocm_lock);

	return IRQ_HANDLED;
}

static void lt9611c_hpd_work(struct work_struct *work)
{
	struct lt9611c *lt9611c = container_of(work, struct lt9611c, work);
	bool connected;

	mutex_lock(&lt9611c->ocm_lock);
	connected = lt9611c->hdmi_connected;
	mutex_unlock(&lt9611c->ocm_lock);

	drm_bridge_hpd_notify(&lt9611c->bridge,
			      connected ? connector_status_connected :
			      connector_status_disconnected);
}

static void lt9611c_reset(struct lt9611c *lt9611c)
{
	gpiod_set_value_cansleep(lt9611c->reset_gpio, 1);
	msleep(20);

	gpiod_set_value_cansleep(lt9611c->reset_gpio, 0);
	msleep(20);

	gpiod_set_value_cansleep(lt9611c->reset_gpio, 1);
	msleep(400);

	dev_dbg(lt9611c->dev, "lt9611c reset");
}

static int lt9611c_regulator_init(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	int ret;

	lt9611c->supplies[0].supply = "vcc";
	lt9611c->supplies[1].supply = "vdd";

	ret = devm_regulator_bulk_get(dev, 2, lt9611c->supplies);

	return ret;
}

static int lt9611c_regulator_enable(struct lt9611c *lt9611c)
{
	int ret;

	ret = regulator_enable(lt9611c->supplies[0].consumer);
	if (ret < 0)
		return ret;

	usleep_range(5000, 10000);

	ret = regulator_enable(lt9611c->supplies[1].consumer);
	if (ret < 0) {
		regulator_disable(lt9611c->supplies[0].consumer);
		return ret;
	}

	return 0;
}

static int lt9611c_regulator_disable(struct lt9611c *lt9611c)
{
	int ret;

	ret = regulator_disable(lt9611c->supplies[0].consumer);
	if (ret < 0)
		return ret;

	ret = regulator_disable(lt9611c->supplies[1].consumer);
	if (ret < 0)
		return ret;

	return 0;
}

static struct mipi_dsi_device *lt9611c_attach_dsi(struct lt9611c *lt9611c,
						  struct device_node *dsi_node)
{
	const struct mipi_dsi_device_info info = { "lt9611c", 0, NULL };
	struct mipi_dsi_device *dsi;
	struct mipi_dsi_host *host;
	struct device *dev = lt9611c->dev;
	int ret;

	host = of_find_mipi_dsi_host_by_node(dsi_node);
	if (!host) {
		dev_err(dev, "failed to find dsi host\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	dsi = devm_mipi_dsi_device_register_full(dev, host, &info);
	if (IS_ERR(dsi)) {
		dev_err(dev, "failed to create dsi device\n");
		return dsi;
	}

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			 MIPI_DSI_MODE_VIDEO_HSE;

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		dev_err(dev, "failed to attach dsi to host\n");
		return ERR_PTR(ret);
	}

	return dsi;
}

static int lt9611c_bridge_attach(struct drm_bridge *bridge,
				 struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);

	return drm_bridge_attach(encoder, lt9611c->next_bridge, bridge, flags);
}

static enum drm_mode_status
lt9611c_hdmi_tmds_char_rate_valid(const struct drm_bridge *bridge,
				  const struct drm_display_mode *mode,
				  unsigned long long tmds_rate)
{
	const struct lt9611c *lt9611c = bridge_to_lt9611c_const(bridge);

	if (lt9611c->chip_type == 2) {
		/*lt9611uxd use*/
		if (tmds_rate > 600000000)
			return MODE_CLOCK_HIGH;

	} else {
		if (tmds_rate > 340000000)
			return MODE_CLOCK_HIGH;
	}

	if (tmds_rate < 25000000)
		return MODE_CLOCK_LOW;

	return MODE_OK;
}

static void lt9611c_video_setup(struct lt9611c *lt9611c,
				const struct drm_display_mode *mode)
{
	struct device *dev = lt9611c->dev;
	int ret;
	u32 h_total, hactive, hsync_len, hfront_porch, hback_porch;
	u32 v_total, vactive, vsync_len, vfront_porch, vback_porch;
	u8 video_timing_set_cmd[26] = {0x57, 0x4d, 0x33, 0x3a};
	u8 return_timing_set_param[3];
	u8 framerate;
	u8 vic = 0x00;

	mutex_lock(&lt9611c->ocm_lock);

	h_total = mode->htotal;
	hactive = mode->hdisplay;
	hsync_len = mode->hsync_end - mode->hsync_start;
	hfront_porch = mode->hsync_start - mode->hdisplay;
	hback_porch = mode->htotal - mode->hsync_end;

	v_total = mode->vtotal;
	vactive = mode->vdisplay;
	vsync_len = mode->vsync_end - mode->vsync_start;
	vfront_porch = mode->vsync_start - mode->vdisplay;
	vback_porch = mode->vtotal - mode->vsync_end;
	framerate = drm_mode_vrefresh(mode);
	vic = drm_match_cea_mode(mode);

	dev_dbg(dev, "hactive=%d, vactive=%d\n", hactive, vactive);
	dev_dbg(dev, "framerate=%d\n", framerate);
	dev_dbg(dev, "vic = 0x%02x\n", vic);

	video_timing_set_cmd[4] = (h_total >> 8) & 0xff;
	video_timing_set_cmd[5] = h_total & 0xff;
	video_timing_set_cmd[6] = (hactive >> 8) & 0xff;
	video_timing_set_cmd[7] = hactive & 0xff;
	video_timing_set_cmd[8] = (hfront_porch >> 8) & 0xff;
	video_timing_set_cmd[9] = hfront_porch & 0xff;
	video_timing_set_cmd[10] = (hsync_len >> 8) & 0xff;
	video_timing_set_cmd[11] = hsync_len & 0xff;
	video_timing_set_cmd[12] = (hback_porch >> 8) & 0xff;
	video_timing_set_cmd[13] = hback_porch & 0xff;
	video_timing_set_cmd[14] = (v_total >> 8) & 0xff;
	video_timing_set_cmd[15] = v_total & 0xff;
	video_timing_set_cmd[16] = (vactive >> 8) & 0xff;
	video_timing_set_cmd[17] = vactive & 0xFF;
	video_timing_set_cmd[18] = (vfront_porch >> 8) & 0xff;
	video_timing_set_cmd[19] = vfront_porch & 0xff;
	video_timing_set_cmd[20] = (vsync_len >> 8) & 0xff;
	video_timing_set_cmd[21] = vsync_len & 0xff;
	video_timing_set_cmd[22] = (vback_porch >> 8) & 0xff;
	video_timing_set_cmd[23] = vback_porch & 0xff;
	video_timing_set_cmd[24] = framerate;
	video_timing_set_cmd[25] = vic;

	ret = lt9611c_read_write_flow(lt9611c,
				      video_timing_set_cmd, 26,
				      return_timing_set_param, 3);
	if (ret)
		dev_err(dev, "video set failed\n");
	mutex_unlock(&lt9611c->ocm_lock);
}

static void lt9611c_bridge_atomic_enable(struct drm_bridge *bridge,
					 struct drm_atomic_state *state)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_display_mode *mode;

	connector = drm_atomic_get_new_connector_for_encoder(state, bridge->encoder);
	if (WARN_ON(!connector))
		return;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (WARN_ON(!conn_state))
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (WARN_ON(!crtc_state))
		return;

	mode = &crtc_state->adjusted_mode;

	lt9611c_video_setup(lt9611c, mode);
}

static enum drm_connector_status
lt9611c_bridge_detect(struct drm_bridge *bridge, struct drm_connector *connector)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);
	struct device *dev = lt9611c->dev;
	int ret;
	bool connected = false;
	u8 cmd[5] = {0x52, 0x48, 0x31, 0x3a, 0x00};
	u8 data[5];

	mutex_lock(&lt9611c->ocm_lock);

	ret = lt9611c_read_write_flow(lt9611c, cmd, 5, data, 5);
	if (ret)
		dev_err(dev, "failed to read HPD status (err=%d)\n", ret);
	else
		connected = (data[4] == 0x02);

	lt9611c->hdmi_connected = connected;

	mutex_unlock(&lt9611c->ocm_lock);

	return connected ? connector_status_connected :
				connector_status_disconnected;
}

static int lt9611c_get_edid_block(void *data, u8 *buf,
				  unsigned int block, size_t len)
{
	struct lt9611c *lt9611c = data;
	struct device *dev = lt9611c->dev;
	u8 cmd[5] = {0x52, 0x48, 0x33, 0x3a, 0x00};
	u8 packet[37];
	int ret, i, offset = 0;

	if (len != 128)
		return -EINVAL;
	mutex_lock(&lt9611c->ocm_lock);

	for (i = 0; i < 4; i++) {
		cmd[4] = block * 4 + i;
		ret = lt9611c_read_write_flow(lt9611c, cmd, sizeof(cmd),
					      packet, sizeof(packet));
		if (ret) {
			dev_err(dev, "Failed to read EDID block %u packet %d\n",
				block, i);
			mutex_unlock(&lt9611c->ocm_lock);
			return ret;
		}

		memcpy(buf + offset, &packet[5], 32);
		offset += 32;
	}

	mutex_unlock(&lt9611c->ocm_lock);

	return 0;
}

static const struct drm_edid *lt9611c_bridge_edid_read(struct drm_bridge *bridge,
						       struct drm_connector *connector)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);

	return drm_edid_read_custom(connector, lt9611c_get_edid_block, lt9611c);
}

static void lt9611c_bridge_hpd_notify(struct drm_bridge *bridge,
				      struct drm_connector *connector,
				      enum drm_connector_status status)
{
	const struct drm_edid *drm_edid;

	if (status == connector_status_disconnected) {
		drm_connector_hdmi_audio_plugged_notify(connector, false);
		drm_edid_connector_update(connector, NULL);
		return;
	}

	drm_edid = lt9611c_bridge_edid_read(bridge, connector);
	drm_edid_connector_update(connector, drm_edid);
	drm_edid_free(drm_edid);

	if (status == connector_status_connected)
		drm_connector_hdmi_audio_plugged_notify(connector, true);
}

static int lt9611c_hdmi_write_audio_infoframe(struct drm_bridge *bridge,
					      const u8 *buffer, size_t len)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);
	u8 audio_infoframe_cmd[16] = {0x57, 0x48, 0x35, 0x3a, 0x02};
	u8 data[5];
	int i, ret;

	for (i = 0; i <  len; i++)
		audio_infoframe_cmd[i + 5] = buffer[i];

	mutex_lock(&lt9611c->ocm_lock);

	ret = lt9611c_read_write_flow(lt9611c, audio_infoframe_cmd, sizeof(audio_infoframe_cmd),
				      data, sizeof(data));
	mutex_unlock(&lt9611c->ocm_lock);

	if (ret < 0) {
		dev_err(lt9611c->dev, "write audio infoframe failed!\n");
		return ret;
	}

	return 0;
}

static int lt9611c_hdmi_clear_audio_infoframe(struct drm_bridge *bridge)
{
	return 0;
}

static int lt9611c_hdmi_write_avi_infoframe(struct drm_bridge *bridge,
					    const u8 *buffer, size_t len)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);
	u8 avi_infoframe_cmd[16] = {0x57, 0x48, 0x35, 0x3a, 0x01};
	u8 data[5];
	int i, ret;

	for (i = 0; i <  len; i++)
		avi_infoframe_cmd[i + 5] = buffer[i];

	mutex_lock(&lt9611c->ocm_lock);

	ret = lt9611c_read_write_flow(lt9611c, avi_infoframe_cmd, sizeof(avi_infoframe_cmd),
				      data, sizeof(data));
	mutex_unlock(&lt9611c->ocm_lock);

	if (ret < 0) {
		dev_err(lt9611c->dev, "write avi infoframe failed!\n");
		return ret;
	}

	return 0;
}

static int lt9611c_hdmi_clear_avi_infoframe(struct drm_bridge *bridge)
{
	return 0;
}

static int lt9611c_hdmi_write_spd_infoframe(struct drm_bridge *bridge,
					    const u8 *buffer, size_t len)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);
	u8 spd_infoframe_cmd[16] = {0x57, 0x48, 0x35, 0x3a, 0x04};
	u8 data[5];
	int i, ret;

	for (i = 0; i <  len; i++)
		spd_infoframe_cmd[i + 5] = buffer[i];

	mutex_lock(&lt9611c->ocm_lock);

	ret = lt9611c_read_write_flow(lt9611c, spd_infoframe_cmd, sizeof(spd_infoframe_cmd),
				      data, sizeof(data));
	mutex_unlock(&lt9611c->ocm_lock);

	if (ret < 0) {
		dev_err(lt9611c->dev, "write spd infoframe failed!\n");
		return ret;
	}

	return 0;
}

static int lt9611c_hdmi_clear_spd_infoframe(struct drm_bridge *bridge)
{
	return 0;
}

static int lt9611c_hdmi_write_hdmi_infoframe(struct drm_bridge *bridge,
					     const u8 *buffer, size_t len)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);
	u8 spd_infoframe_cmd[16] = {0x57, 0x48, 0x35, 0x3a, 0x05};
	u8 data[5];
	int i, ret;

	for (i = 0; i <  len; i++)
		spd_infoframe_cmd[i + 5] = buffer[i];

	mutex_lock(&lt9611c->ocm_lock);

	ret = lt9611c_read_write_flow(lt9611c, spd_infoframe_cmd, sizeof(spd_infoframe_cmd),
				      data, sizeof(data));
	mutex_unlock(&lt9611c->ocm_lock);
	if (ret < 0) {
		dev_err(lt9611c->dev, "write hdmi infoframe failed!\n");
		return ret;
	}

	return 0;
}

static int lt9611c_hdmi_clear_hdmi_infoframe(struct drm_bridge *bridge)
{
	return 0;
}

static int lt9611c_hdmi_audio_prepare(struct drm_bridge *bridge,
				      struct drm_connector *connector,
				      struct hdmi_codec_daifmt *fmt,
				      struct hdmi_codec_params *hparms)
{
	struct lt9611c *lt9611c = bridge_to_lt9611c(bridge);
	u8 audio_cmd[6] = {0x57, 0x48, 0x36, 0x3a};
	u8 data[5];
	int ret;

	/* Validate sample rate and width (LT9611C auto-detects but we still check) */
	switch (hparms->sample_rate) {
	case 32000:
	case 44100:
	case 48000:
	case 88200:
	case 96000:
	case 176400:
	case 192000:
		break;
	default:
		return -EINVAL;
	}

	switch (hparms->sample_width) {
	case 16:
	case 18:
	case 20:
	case 24:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt->fmt) {
	case HDMI_I2S:
		audio_cmd[4] = 0x01;
		break;
	case HDMI_SPDIF:
		audio_cmd[4] = 0x02;
		break;
	default:
		return -EINVAL;
	}

	audio_cmd[5] = hparms->channels;

	mutex_lock(&lt9611c->ocm_lock);
	ret = lt9611c_read_write_flow(lt9611c, audio_cmd, sizeof(audio_cmd),
				      data, sizeof(data));
	mutex_unlock(&lt9611c->ocm_lock);

	if (ret < 0) {
		dev_err(lt9611c->dev, "set audio info failed!\n");
		return ret;
	}

	return drm_atomic_helper_connector_hdmi_update_audio_infoframe(connector,
									&hparms->cea);
}

static void lt9611c_hdmi_audio_shutdown(struct drm_bridge *bridge,
					struct drm_connector *connector)
{
	drm_atomic_helper_connector_hdmi_clear_audio_infoframe(connector);
}

static int lt9611c_hdmi_audio_startup(struct drm_bridge *bridge,
				      struct drm_connector *connector)
{
	return 0;
}

static const struct drm_bridge_funcs lt9611c_bridge_funcs = {
	.attach = lt9611c_bridge_attach,
	.detect = lt9611c_bridge_detect,
	.edid_read = lt9611c_bridge_edid_read,
	.hpd_notify = lt9611c_bridge_hpd_notify,
	.atomic_enable = lt9611c_bridge_atomic_enable,

	.hdmi_tmds_char_rate_valid = lt9611c_hdmi_tmds_char_rate_valid,
	.hdmi_write_audio_infoframe = lt9611c_hdmi_write_audio_infoframe,
	.hdmi_clear_audio_infoframe = lt9611c_hdmi_clear_audio_infoframe,
	.hdmi_write_avi_infoframe = lt9611c_hdmi_write_avi_infoframe,
	.hdmi_clear_avi_infoframe = lt9611c_hdmi_clear_avi_infoframe,
	.hdmi_write_spd_infoframe = lt9611c_hdmi_write_spd_infoframe,
	.hdmi_clear_spd_infoframe = lt9611c_hdmi_clear_spd_infoframe,
	.hdmi_write_hdmi_infoframe = lt9611c_hdmi_write_hdmi_infoframe,
	.hdmi_clear_hdmi_infoframe = lt9611c_hdmi_clear_hdmi_infoframe,
	.hdmi_audio_startup = lt9611c_hdmi_audio_startup,
	.hdmi_audio_prepare = lt9611c_hdmi_audio_prepare,
	.hdmi_audio_shutdown = lt9611c_hdmi_audio_shutdown,
};

static int lt9611c_parse_dt(struct device *dev,
			    struct lt9611c *lt9611c)
{
	lt9611c->dsi0_node = of_graph_get_remote_node(dev->of_node, 0, -1);
	if (!lt9611c->dsi0_node) {
		dev_err(dev, "failed to get remote node for primary dsi\n");
		return -ENODEV;
	}

	lt9611c->dsi1_node = of_graph_get_remote_node(dev->of_node, 1, -1);

	return drm_of_find_panel_or_bridge(dev->of_node, 2, -1, NULL, &lt9611c->next_bridge);
}

static int lt9611c_gpio_init(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;

	lt9611c->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(lt9611c->reset_gpio)) {
		dev_err(dev, "failed to acquire reset gpio\n");
		return PTR_ERR(lt9611c->reset_gpio);
	}

	return 0;
}

static int lt9611c_read_version(struct lt9611c *lt9611c)
{
	u8 buf[2];
	int ret;

	ret = regmap_write(lt9611c->regmap, 0xe0ee, 0x01);
	if (ret)
		return ret;

	ret = regmap_bulk_read(lt9611c->regmap, 0xe080, buf, 2);
	if (ret)
		return ret;

	return (buf[0] << 8) | buf[1];
}

static int lt9611c_read_chipid(struct lt9611c *lt9611c)
{
	struct device *dev = lt9611c->dev;
	u8 chipid[2];
	int ret;

	ret = regmap_write(lt9611c->regmap, 0xe0ee, 0x01);
	if (ret)
		return ret;

	ret = regmap_bulk_read(lt9611c->regmap, 0xe100, chipid, 2);
	if (ret)
		return ret;

	if (chipid[0] != 0x23 || chipid[1] != 0x06) {
		dev_err(dev, "ChipID: 0x%02x 0x%02x\n", chipid[0], chipid[1]);
		return -ENODEV;
	}

	return 0;
}

static ssize_t lt9611c_firmware_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t len)
{
	struct lt9611c *lt9611c = dev_get_drvdata(dev);
	int ret;

	lt9611c_lock(lt9611c);

	ret = lt9611c_firmware_upgrade(lt9611c);
	if (ret < 0) {
		dev_err(dev, "upgrade failure\n");
		goto out;
	}
	lt9611c_reset(lt9611c);
	ret = lt9611c_upgrade_result(lt9611c);
	if (ret < 0)
		goto out;

out:
	lt9611c_unlock(lt9611c);
	lt9611c_reset(lt9611c);
	if (lt9611c->fw) {
		release_firmware(lt9611c->fw);
		lt9611c->fw = NULL;
	}

	return ret < 0 ? ret : len;
}

static ssize_t lt9611c_firmware_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lt9611c *lt9611c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%04x\n", lt9611c->fw_version);
}

static DEVICE_ATTR_RW(lt9611c_firmware);

static struct attribute *lt9611c_attrs[] = {
	&dev_attr_lt9611c_firmware.attr,
	NULL,
};

static const struct attribute_group lt9611c_attr_group = {
	.attrs = lt9611c_attrs,
};

static const struct attribute_group *lt9611c_attr_groups[] = {
	&lt9611c_attr_group,
	NULL,
};

static int lt9611c_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	struct lt9611c *lt9611c;
	struct device *dev = &client->dev;
	bool fw_updated = false;
	int ret;

	crc8_populate_msb(lt9611c_crc8_table, LT9611C_CRC_POLYNOMIAL);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "device doesn't support I2C\n");
		return -ENODEV;
	}

	lt9611c = devm_drm_bridge_alloc(dev, struct lt9611c, bridge, &lt9611c_bridge_funcs);
	if (IS_ERR(lt9611c))
		return PTR_ERR(lt9611c);

	lt9611c->dev = dev;
	lt9611c->client = client;
	lt9611c->chip_type = id->driver_data;
	mutex_init(&lt9611c->ocm_lock);

	lt9611c->regmap = devm_regmap_init_i2c(client, &lt9611c_regmap_config);
	if (IS_ERR(lt9611c->regmap)) {
		dev_err(dev, "regmap i2c init failed\n");
		return PTR_ERR(lt9611c->regmap);
	}

	ret = lt9611c_parse_dt(dev, lt9611c);
	if (ret) {
		dev_err(dev, "failed to parse device tree\n");
		return ret;
	}

	ret = lt9611c_gpio_init(lt9611c);
	if (ret < 0)
		goto err_of_put;

	ret = lt9611c_regulator_init(lt9611c);
	if (ret < 0)
		goto err_of_put;

	ret = lt9611c_regulator_enable(lt9611c);
	if (ret)
		goto err_of_put;

	lt9611c_reset(lt9611c);

	lt9611c_lock(lt9611c);

	ret = lt9611c_read_chipid(lt9611c);
	if (ret < 0) {
		dev_err(dev, "failed to read chip id.\n");
		lt9611c_unlock(lt9611c);
		goto err_disable_regulators;
	}

retry:
	ret = lt9611c_read_version(lt9611c);
	if (ret < 0) {
		dev_err(dev, "failed to read fw version\n");
		lt9611c_unlock(lt9611c);
		goto err_disable_regulators;

	} else if (ret == 0) {
		if (!fw_updated) {
			fw_updated = true;
			ret = lt9611c_firmware_upgrade(lt9611c);
			if (ret < 0) {
				lt9611c_unlock(lt9611c);
				goto err_disable_regulators;
			}

			lt9611c_reset(lt9611c);

			ret = lt9611c_upgrade_result(lt9611c);
			if (ret < 0) {
				lt9611c_unlock(lt9611c);
				goto err_disable_regulators;
			}

			goto retry;

		} else {
			dev_err(dev, "fw version 0x%04x, update failed\n", ret);
			ret = -EOPNOTSUPP;
			lt9611c_unlock(lt9611c);
			goto err_disable_regulators;
		}
	}

	lt9611c_unlock(lt9611c);
	lt9611c->fw_version = ret;

	dev_dbg(dev, "current version:0x%04x", lt9611c->fw_version);

	INIT_WORK(&lt9611c->work, lt9611c_hpd_work);

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					lt9611c_irq_thread_handler,
					IRQF_TRIGGER_FALLING |
					IRQF_ONESHOT |
					IRQF_NO_AUTOEN,
					"lt9611c", lt9611c);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		goto err_disable_regulators;
	}

	lt9611c->bridge.of_node = client->dev.of_node;
	lt9611c->bridge.ops = DRM_BRIDGE_OP_DETECT |
			DRM_BRIDGE_OP_EDID |
			DRM_BRIDGE_OP_HPD |
			DRM_BRIDGE_OP_HDMI_AUDIO;
	lt9611c->bridge.type = DRM_MODE_CONNECTOR_HDMIA;

	lt9611c->bridge.hdmi_audio_dev = dev;
	lt9611c->bridge.hdmi_audio_max_i2s_playback_channels = 8;
	lt9611c->bridge.hdmi_audio_dai_port = 2;

	drm_bridge_add(&lt9611c->bridge);

	/* Attach primary DSI */
	lt9611c->dsi0 = lt9611c_attach_dsi(lt9611c, lt9611c->dsi0_node);
	if (IS_ERR(lt9611c->dsi0)) {
		ret = PTR_ERR(lt9611c->dsi0);
		goto err_remove_bridge;
	}

	/* Attach secondary DSI, if specified */
	if (lt9611c->dsi1_node) {
		lt9611c->dsi1 = lt9611c_attach_dsi(lt9611c, lt9611c->dsi1_node);
		if (IS_ERR(lt9611c->dsi1)) {
			ret = PTR_ERR(lt9611c->dsi1);
			goto err_remove_bridge;
		}
	}

	lt9611c->hdmi_connected = false;
	i2c_set_clientdata(client, lt9611c);
	enable_irq(client->irq);
	lt9611c_reset(lt9611c);

	return 0;

err_remove_bridge:
	free_irq(client->irq, lt9611c);
	cancel_work_sync(&lt9611c->work);
	drm_bridge_remove(&lt9611c->bridge);

err_disable_regulators:
	regulator_bulk_disable(ARRAY_SIZE(lt9611c->supplies), lt9611c->supplies);

err_of_put:
	of_node_put(lt9611c->dsi1_node);
	of_node_put(lt9611c->dsi0_node);
	if (lt9611c->fw) {
		release_firmware(lt9611c->fw);
		lt9611c->fw = NULL;
	}

	return ret;
}

static void lt9611c_remove(struct i2c_client *client)
{
	struct lt9611c *lt9611c = i2c_get_clientdata(client);

	free_irq(client->irq, lt9611c);
	cancel_work_sync(&lt9611c->work);
	drm_bridge_remove(&lt9611c->bridge);
	mutex_destroy(&lt9611c->ocm_lock);
	regulator_bulk_disable(ARRAY_SIZE(lt9611c->supplies), lt9611c->supplies);
	of_node_put(lt9611c->dsi1_node);
	of_node_put(lt9611c->dsi0_node);
}

static int lt9611c_bridge_suspend(struct device *dev)
{
	struct lt9611c *lt9611c = dev_get_drvdata(dev);
	int ret;

	dev_dbg(lt9611c->dev, "suspend\n");
	disable_irq(lt9611c->client->irq);
	ret = lt9611c_regulator_disable(lt9611c);
	gpiod_set_value_cansleep(lt9611c->reset_gpio, 0);

	return ret;
}

static int lt9611c_bridge_resume(struct device *dev)
{
	struct lt9611c *lt9611c = dev_get_drvdata(dev);
	int ret;

	ret = lt9611c_regulator_enable(lt9611c);
	enable_irq(lt9611c->client->irq);
	lt9611c_reset(lt9611c);
	dev_dbg(lt9611c->dev, "resume\n");

	return ret;
}

static const struct dev_pm_ops lt9611c_bridge_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(lt9611c_bridge_suspend,
				lt9611c_bridge_resume)
};

static struct i2c_device_id lt9611c_id[] = {
	/* chip_type */
	{ "lontium,lt9611c", 0 },
	{ "lontium,lt9611ex", 1 },
	{ "lontium,lt9611uxd", 2 },
	{ /* sentinel */ }
};

static const struct of_device_id lt9611c_match_table[] = {
	{ .compatible = "lontium,lt9611c" },
	{ .compatible = "lontium,lt9611ex" },
	{ .compatible = "lontium,lt9611uxd" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lt9611c_match_table);

static struct i2c_driver lt9611c_driver = {
	.driver = {
		.name = "lt9611c",
		.of_match_table = lt9611c_match_table,
		.pm = &lt9611c_bridge_pm_ops,
		.dev_groups = lt9611c_attr_groups,
	},
	.probe = lt9611c_probe,
	.remove = lt9611c_remove,
	.id_table = lt9611c_id,
};
module_i2c_driver(lt9611c_driver);

MODULE_AUTHOR("SunYun Yang <syyang@lontium.com>");
MODULE_DESCRIPTION("Lontium lt9611c mipi-dsi to hdmi driver");
MODULE_LICENSE("GPL v2");

