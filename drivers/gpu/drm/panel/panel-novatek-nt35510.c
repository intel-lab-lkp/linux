// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT35510 panel driver
 * Copyright (C) 2020 Linus Walleij <linus.walleij@linaro.org>
 * Based on code by Robert Teather (C) 2012 Samsung
 *
 * This display driver (and I refer to the physical component NT35510,
 * not this Linux kernel software driver) can handle:
 * 480x864, 480x854, 480x800, 480x720 and 480x640 pixel displays.
 * It has 480x840x24bit SRAM embedded for storing a frame.
 * When powered on the display is by default in 480x800 mode.
 *
 * The actual panels using this component have different names, but
 * the code needed to set up and configure the panel will be similar,
 * so they should all use the NT35510 driver with appropriate configuration
 * per-panel, e.g. for physical size.
 *
 * This driver is for the DSI interface to panels using the NT35510.
 *
 * The NT35510 can also use an RGB (DPI) interface combined with an
 * I2C or SPI interface for setting up the NT35510. If this is needed
 * this panel driver should be refactored to also support that use
 * case.
 */
#include <linux/backlight.h>
#include <linux/bitops.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define MCS_CMD_READ_ID1	0xDA
#define MCS_CMD_READ_ID2	0xDB
#define MCS_CMD_READ_ID3	0xDC
#define MCS_CMD_MTP_READ_SETTING 0xF8 /* Uncertain about name */
#define MCS_CMD_MTP_READ_PARAM 0xFF /* Uncertain about name */

#define _INIT_DCS_CMD(...) { \
	.type = INIT_DCS_CMD, \
	.len = sizeof((u8[]){__VA_ARGS__}), \
	.data = (u8[]){__VA_ARGS__} }

#define _INIT_DELAY_CMD(...) { \
	.type = DELAY_CMD,\
	.len = sizeof((u8[]){__VA_ARGS__}), \
	.data = (u8[]){__VA_ARGS__} }

enum dsi_cmd_type {
	INIT_DCS_CMD,
	DELAY_CMD,
};

struct panel_init_cmd {
	enum dsi_cmd_type type;
	size_t len;
	const u8 *data;
};

struct nt35510_config {
	/**
	 * @width_mm: physical panel width [mm]
	 */
	u32 width_mm;
	/**
	 * @height_mm: physical panel height [mm]
	 */
	u32 height_mm;
	/**
	 * @mode: the display mode. This is only relevant outside the panel
	 * in video mode: in command mode this is configuring the internal
	 * timing in the display controller.
	 */
	const struct drm_display_mode mode;
	/**
	* @lanes: number of active data lanes
	*/
	unsigned int lanes;
	/**
	 * @format: pixel format for video mode
	 */
	enum mipi_dsi_pixel_format format;
	/**
	 * @mode_flags: DSI operation mode related flags
	 */
	unsigned long mode_flags;
	/**
	 * @hs_rate: maximum lane frequency for high speed mode in hertz, this should
	 * be set to the real limits of the hardware, zero is only accepted for
	 * legacy drivers
	 */
	unsigned long hs_rate;
	/**
	 * @lp_rate: maximum lane frequency for low power mode in hertz, this should
	 * be set to the real limits of the hardware, zero is only accepted for
	 * legacy drivers
	 */
	unsigned long lp_rate;
	/**
	 * @init_cmds: initialization command list
	 */
	const struct panel_init_cmd *init_cmds;
};

static const struct panel_init_cmd hydis_hva40wv1_init_cmds[] = {
	_INIT_DCS_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	_INIT_DCS_CMD(0xB0, 0x09, 0x09, 0x09),
	_INIT_DCS_CMD(0xB6, 0x34, 0x34, 0x34),
	_INIT_DCS_CMD(0xB1, 0x09, 0x09, 0x09),
	_INIT_DCS_CMD(0xB7, 0x24, 0x24, 0x24),
	_INIT_DCS_CMD(0xB3, 0x05, 0x05, 0x05),
	_INIT_DCS_CMD(0xB9, 0x24, 0x24, 0x24),
	_INIT_DCS_CMD(0xBF, 0x01),
	_INIT_DCS_CMD(0xB5, 0x0B, 0x0B, 0x0B),
	_INIT_DCS_CMD(0xBA, 0x24, 0x24, 0x24),
	_INIT_DCS_CMD(0xBC, 0x00, 0xA3, 0x00),
	_INIT_DCS_CMD(0xBD, 0x00, 0xA3, 0x00),
	_INIT_DELAY_CMD(0x0A),
	_INIT_DCS_CMD(0xD1, 0x00, 0x01, 0x00, 0x43, 0x00, 0x6B, 0x00, 0x87, 0x00, 0xA3, 0x00, 0xCE, 0x00, 0xF1, 0x01),
	_INIT_DCS_CMD(0x27, 0x01, 0x53, 0x01, 0x98, 0x01, 0xCE, 0x02, 0x22, 0x02, 0x83, 0x02, 0x78, 0x02, 0x9E),
	_INIT_DCS_CMD(0x02, 0xDD, 0x03, 0x00, 0x03, 0x2E, 0x03, 0x54, 0x03, 0x7F, 0x03, 0x95, 0x03, 0xB3, 0x03),
	_INIT_DCS_CMD(0xC2, 0x03, 0xE1, 0x03, 0xF1, 0x03, 0xFE),
	_INIT_DCS_CMD(0xD2, 0x00, 0x01, 0x00, 0x43, 0x00, 0x6B, 0x00, 0x87, 0x00, 0xA3, 0x00, 0xCE, 0x00, 0xF1, 0x01),
	_INIT_DCS_CMD(0x27, 0x01, 0x53, 0x01, 0x98, 0x01, 0xCE, 0x02, 0x22, 0x02, 0x83, 0x02, 0x78, 0x02, 0x9E),
	_INIT_DCS_CMD(0x02, 0xDD, 0x03, 0x00, 0x03, 0x2E, 0x03, 0x54, 0x03, 0x7F, 0x03, 0x95, 0x03, 0xB3, 0x03),
	_INIT_DCS_CMD(0xC2, 0x03, 0xE1, 0x03, 0xF1, 0x03, 0xFE),
	_INIT_DCS_CMD(0xD3, 0x00, 0x01, 0x00, 0x43, 0x00, 0x6B, 0x00, 0x87, 0x00, 0xA3, 0x00, 0xCE, 0x00, 0xF1, 0x01),
	_INIT_DCS_CMD(0x27, 0x01, 0x53, 0x01, 0x98, 0x01, 0xCE, 0x02, 0x22, 0x02, 0x83, 0x02, 0x78, 0x02, 0x9E),
	_INIT_DCS_CMD(0x02, 0xDD, 0x03, 0x00, 0x03, 0x2E, 0x03, 0x54, 0x03, 0x7F, 0x03, 0x95, 0x03, 0xB3, 0x03),
	_INIT_DCS_CMD(0xC2, 0x03, 0xE1, 0x03, 0xF1, 0x03, 0xFE),
	_INIT_DCS_CMD(0xD4, 0x00, 0x01, 0x00, 0x43, 0x00, 0x6B, 0x00, 0x87, 0x00, 0xA3, 0x00, 0xCE, 0x00, 0xF1, 0x01),
	_INIT_DCS_CMD(0x27, 0x01, 0x53, 0x01, 0x98, 0x01, 0xCE, 0x02, 0x22, 0x02, 0x43, 0x02, 0x50, 0x02, 0x9E),
	_INIT_DCS_CMD(0x02, 0xDD, 0x03, 0x00, 0x03, 0x2E, 0x03, 0x54, 0x03, 0x7F, 0x03, 0x95, 0x03, 0xB3, 0x03),
	_INIT_DCS_CMD(0xC2, 0x03, 0xE1, 0x03, 0xF1, 0x03, 0xFE),
	_INIT_DCS_CMD(0xD5, 0x00, 0x01, 0x00, 0x43, 0x00, 0x6B, 0x00, 0x87, 0x00, 0xA3, 0x00, 0xCE, 0x00, 0xF1, 0x01),
	_INIT_DCS_CMD(0x27, 0x01, 0x53, 0x01, 0x98, 0x01, 0xCE, 0x02, 0x22, 0x02, 0x43, 0x02, 0x50, 0x02, 0x9E),
	_INIT_DCS_CMD(0x02, 0xDD, 0x03, 0x00, 0x03, 0x2E, 0x03, 0x54, 0x03, 0x7F, 0x03, 0x95, 0x03, 0xB3, 0x03),
	_INIT_DCS_CMD(0xC2, 0x03, 0xE1, 0x03, 0xF1, 0x03, 0xFE),
	_INIT_DCS_CMD(0xD6, 0x00, 0x01, 0x00, 0x43, 0x00, 0x6B, 0x00, 0x87, 0x00, 0xA3, 0x00, 0xCE, 0x00, 0xF1, 0x01),
	_INIT_DCS_CMD(0x27, 0x01, 0x53, 0x01, 0x98, 0x01, 0xCE, 0x02, 0x22, 0x02, 0x43, 0x02, 0x50, 0x02, 0x9E),
	_INIT_DCS_CMD(0x02, 0xDD, 0x03, 0x00, 0x03, 0x2E, 0x03, 0x54, 0x03, 0x7F, 0x03, 0x95, 0x03, 0xB3, 0x03),
	_INIT_DCS_CMD(0xC2, 0x03, 0xE1, 0x03, 0xF1, 0x03, 0xFE),
	_INIT_DCS_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00),
	_INIT_DCS_CMD(0xB1, 0x4C, 0x04),
	_INIT_DCS_CMD(0x36, 0x02),
	_INIT_DCS_CMD(0xB6, 0x0A),
	_INIT_DCS_CMD(0xB7, 0x00, 0x00),
	_INIT_DCS_CMD(0xB8, 0x01, 0x05, 0x05, 0x05),
	_INIT_DCS_CMD(0xBA, 0x01),
	_INIT_DCS_CMD(0xBD, 0x01, 0x84, 0x07, 0x32, 0x00),
	_INIT_DCS_CMD(0xBE, 0x01, 0x84, 0x07, 0x31, 0x00),
	_INIT_DCS_CMD(0xBF, 0x01, 0x84, 0x07, 0x31, 0x00),
	_INIT_DCS_CMD(0x35, 0x00),
	_INIT_DCS_CMD(0xCC, 0x03, 0x00, 0x00),
	_INIT_DCS_CMD(0x11),
	_INIT_DELAY_CMD(0x78),
	_INIT_DCS_CMD(0x29),
	_INIT_DELAY_CMD(0x0A),
	{},
};

/**
 * struct nt35510 - state container for the NT35510 panel
 */
struct nt35510 {
	/**
	 * @dev: the container device
	 */
	struct device *dev;
	/**
	 * @conf: the specific panel configuration, as the NT35510
	 * can be combined with many physical panels, they can have
	 * different physical dimensions and gamma correction etc,
	 * so this is stored in the config.
	 */
	const struct nt35510_config *conf;
	/**
	 * @panel: the DRM panel object for the instance
	 */
	struct drm_panel panel;
	/**
	 * @supplies: regulators supplying the panel
	 */
	struct regulator_bulk_data supplies[2];
	/**
	 * @reset_gpio: the reset line
	 */
	struct gpio_desc *reset_gpio;
};

/* Manufacturer command has strictly this byte sequence */
static const u8 nt35510_mauc_mtp_read_param[] = { 0xAA, 0x55, 0x25, 0x01 };
static const u8 nt35510_mauc_mtp_read_setting[] = { 0x01, 0x02, 0x00, 0x20,
						    0x33, 0x13, 0x00, 0x40,
						    0x00, 0x00, 0x23, 0x02 };
static inline struct nt35510 *panel_to_nt35510(struct drm_panel *panel)
{
	return container_of(panel, struct nt35510, panel);
}

static int nt35510_send_long(struct nt35510 *nt, struct mipi_dsi_device *dsi,
			     u8 cmd, u8 cmdlen, const u8 *seq)
{
	const u8 *seqp = seq;
	int cmdwritten = 0;
	int chunk = cmdlen;
	int ret;

	if (chunk > 15)
		chunk = 15;
	ret = mipi_dsi_dcs_write(dsi, cmd, seqp, chunk);
	if (ret < 0) {
		dev_err(nt->dev, "error sending DCS command seq cmd %02x\n", cmd);
		return ret;
	}
	cmdwritten += chunk;
	seqp += chunk;

	while (cmdwritten < cmdlen) {
		chunk = cmdlen - cmdwritten;
		if (chunk > 15)
			chunk = 15;
		ret = mipi_dsi_generic_write(dsi, seqp, chunk);
		if (ret < 0) {
			dev_err(nt->dev, "error sending generic write seq %02x\n", cmd);
			return ret;
		}
		cmdwritten += chunk;
		seqp += chunk;
	}
	dev_dbg(nt->dev, "sent command %02x %02x bytes\n", cmd, cmdlen);
	return 0;
}

static int nt35510_read_id(struct nt35510 *nt)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(nt->dev);
	u8 id1, id2, id3;
	int ret;

	ret = mipi_dsi_dcs_read(dsi, MCS_CMD_READ_ID1, &id1, 1);
	if (ret < 0) {
		dev_err(nt->dev, "could not read MTP ID1\n");
		return ret;
	}
	ret = mipi_dsi_dcs_read(dsi, MCS_CMD_READ_ID2, &id2, 1);
	if (ret < 0) {
		dev_err(nt->dev, "could not read MTP ID2\n");
		return ret;
	}
	ret = mipi_dsi_dcs_read(dsi, MCS_CMD_READ_ID3, &id3, 1);
	if (ret < 0) {
		dev_err(nt->dev, "could not read MTP ID3\n");
		return ret;
	}

	/*
	 * Multi-Time Programmable (?) memory contains manufacturer
	 * ID (e.g. Hydis 0x55), driver ID (e.g. NT35510 0xc0) and
	 * version.
	 */
	dev_info(nt->dev, "MTP ID manufacturer: %02x version: %02x driver: %02x\n", id1, id2, id3);

	return 0;
}

static int nt35510_set_brightness(struct backlight_device *bl)
{
	struct nt35510 *nt = bl_get_data(bl);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(nt->dev);
	u8 brightness = bl->props.brightness;
	int ret;

	dev_dbg(nt->dev, "set brightness %d\n", brightness);
	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				 &brightness,
				 sizeof(brightness));
	if (ret < 0)
		return ret;

	return 0;
}

static const struct backlight_ops nt35510_bl_ops = {
	.update_status = nt35510_set_brightness,
};

/*
 * This power-on sequence
 */
static int nt35510_power_on(struct nt35510 *nt)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(nt->supplies), nt->supplies);
	if (ret < 0) {
		dev_err(nt->dev, "unable to enable regulators\n");
		return ret;
	}

	/* Toggle RESET in accordance with datasheet page 370 */
	if (nt->reset_gpio) {
		gpiod_set_value(nt->reset_gpio, 1);
		/* Active min 10 us according to datasheet, let's say 20 */
		usleep_range(20, 1000);
		gpiod_set_value(nt->reset_gpio, 0);
		/*
		 * 5 ms during sleep mode, 120 ms during sleep out mode
		 * according to datasheet, let's use 120-140 ms.
		 */
		usleep_range(120000, 140000);
	}

	return 0;
}

static int nt35510_power_off(struct nt35510 *nt)
{
	int ret;

	ret = regulator_bulk_disable(ARRAY_SIZE(nt->supplies), nt->supplies);
	if (ret)
		return ret;

	if (nt->reset_gpio)
		gpiod_set_value(nt->reset_gpio, 1);

	return 0;
}

static int nt35510_unprepare(struct drm_panel *panel)
{
	struct nt35510 *nt = panel_to_nt35510(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(nt->dev);
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret) {
		dev_err(nt->dev, "failed to turn display off (%d)\n", ret);
		return ret;
	}
	usleep_range(10000, 20000);

	/* Enter sleep mode */
	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret) {
		dev_err(nt->dev, "failed to enter sleep mode (%d)\n", ret);
		return ret;
	}

	/* Wait 4 frames, how much is that 5ms in the vendor driver */
	usleep_range(5000, 10000);

	ret = nt35510_power_off(nt);
	if (ret)
		return ret;

	return 0;
}

static int nt35510_init(struct nt35510 *nt)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(nt->dev);
	const struct panel_init_cmd *cmds = nt->conf->init_cmds;
	int i, ret;

	if (!cmds) {
		dev_warn(nt->dev, "empty initialization command list\n");
		return 0;
	}

	for (i = 0; cmds[i].len != 0; i++) {
		const struct panel_init_cmd *cmd = &cmds[i];

		switch (cmd->type) {
		case DELAY_CMD:
			msleep(cmd->data[0]);
			ret = 0;
			break;
		case INIT_DCS_CMD:
			ret = nt35510_send_long(nt, dsi, cmd->data[0],
						cmd->len - 1,
						cmd->len <= 1 ? NULL :
						&cmd->data[1]);
			break;
		default:
			ret = -EINVAL;
		}

		if (ret < 0) {
			dev_err(nt->dev, "failed to write command %u\n", i);
			return ret;
		}
	}

	return 0;
}

static int nt35510_prepare(struct drm_panel *panel)
{
	struct nt35510 *nt = panel_to_nt35510(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(nt->dev);
	int ret;

	ret = nt35510_power_on(nt);
	if (ret)
		return ret;

	ret = nt35510_send_long(nt, dsi, MCS_CMD_MTP_READ_PARAM,
				ARRAY_SIZE(nt35510_mauc_mtp_read_param),
				nt35510_mauc_mtp_read_param);
	if (ret)
		return ret;

	ret = nt35510_send_long(nt, dsi, MCS_CMD_MTP_READ_SETTING,
				ARRAY_SIZE(nt35510_mauc_mtp_read_setting),
				nt35510_mauc_mtp_read_setting);
	if (ret)
		return ret;

	nt35510_read_id(nt);

	return nt35510_init(nt);
}

static int nt35510_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct nt35510 *nt = panel_to_nt35510(panel);
	struct drm_display_mode *mode;
	struct drm_display_info *info;

	info = &connector->display_info;
	info->width_mm = nt->conf->width_mm;
	info->height_mm = nt->conf->height_mm;
	mode = drm_mode_duplicate(connector->dev, &nt->conf->mode);
	if (!mode) {
		dev_err(panel->dev, "bad mode or failed to add mode\n");
		return -EINVAL;
	}
	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	mode->width_mm = nt->conf->width_mm;
	mode->height_mm = nt->conf->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1; /* Number of modes */
}

static const struct drm_panel_funcs nt35510_drm_funcs = {
	.unprepare = nt35510_unprepare,
	.prepare = nt35510_prepare,
	.get_modes = nt35510_get_modes,
};

static int nt35510_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct nt35510 *nt;
	int ret;

	nt = devm_kzalloc(dev, sizeof(struct nt35510), GFP_KERNEL);
	if (!nt)
		return -ENOMEM;

	/*
	 * Every new incarnation of this display must have a unique
	 * data entry for the system in this driver.
	 */
	nt->conf = of_device_get_match_data(dev);
	if (!nt->conf) {
		dev_err(dev, "missing device configuration\n");
		return -ENODEV;
	}

	mipi_dsi_set_drvdata(dsi, nt);
	nt->dev = dev;

	dsi->lanes = nt->conf->lanes;
	dsi->format = nt->conf->format;
	dsi->hs_rate = nt->conf->hs_rate;
	dsi->lp_rate = nt->conf->lp_rate;
	dsi->mode_flags = nt->conf->mode_flags;

	nt->supplies[0].supply = "vdd"; /* 2.3-4.8 V */
	nt->supplies[1].supply = "vddi"; /* 1.65-3.3V */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(nt->supplies),
				      nt->supplies);
	if (ret < 0)
		return ret;
	ret = regulator_set_voltage(nt->supplies[0].consumer,
				    2300000, 4800000);
	if (ret)
		return ret;
	ret = regulator_set_voltage(nt->supplies[1].consumer,
				    1650000, 3300000);
	if (ret)
		return ret;

	nt->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(nt->reset_gpio)) {
		dev_err(dev, "error getting RESET GPIO\n");
		return PTR_ERR(nt->reset_gpio);
	}

	drm_panel_init(&nt->panel, dev, &nt35510_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	/*
	 * First, try to locate an external backlight (such as on GPIO)
	 * if this fails, assume we will want to use the internal backlight
	 * control.
	 */
	ret = drm_panel_of_backlight(&nt->panel);
	if (ret) {
		dev_err(dev, "error getting external backlight %d\n", ret);
		return ret;
	}
	if (!nt->panel.backlight) {
		struct backlight_device *bl;

		bl = devm_backlight_device_register(dev, "nt35510", dev, nt,
						    &nt35510_bl_ops, NULL);
		if (IS_ERR(bl)) {
			dev_err(dev, "failed to register backlight device\n");
			return PTR_ERR(bl);
		}
		bl->props.max_brightness = 255;
		bl->props.brightness = 255;
		bl->props.power = FB_BLANK_POWERDOWN;
		nt->panel.backlight = bl;
	}

	drm_panel_add(&nt->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&nt->panel);

	return 0;
}

static void nt35510_remove(struct mipi_dsi_device *dsi)
{
	struct nt35510 *nt = mipi_dsi_get_drvdata(dsi);
	int ret;

	mipi_dsi_detach(dsi);
	/* Power off */
	ret = nt35510_power_off(nt);
	if (ret)
		dev_err(&dsi->dev, "Failed to power off\n");

	drm_panel_remove(&nt->panel);
}

/*
 * The Hydis HVA40WV1 panel
 */
static const struct nt35510_config nt35510_hydis_hva40wv1 = {
	.width_mm = 52,
	.height_mm = 86,
	/**
	 * As the Hydis panel is used in command mode, the porches etc
	 * are settings programmed internally into the NT35510 controller
	 * and generated toward the physical display. As the panel is not
	 * used in video mode, these are not really exposed to the DSI
	 * host.
	 *
	 * Display frame rate control:
	 * Frame rate = (20 MHz / 1) / (389 * (7 + 50 + 800)) ~= 60 Hz
	 */
	.mode = {
		/* The internal pixel clock of the NT35510 is 20 MHz */
		.clock = 20000,
		.hdisplay = 480,
		.hsync_start = 480 + 2, /* HFP = 2 */
		.hsync_end = 480 + 2 + 0, /* HSync = 0 */
		.htotal = 480 + 2 + 0 + 5, /* HFP = 5 */
		.vdisplay = 800,
		.vsync_start = 800 + 2, /* VFP = 2 */
		.vsync_end = 800 + 2 + 0, /* VSync = 0 */
		.vtotal = 800 + 2 + 0 + 5, /* VBP = 5 */
		.flags = 0,
	},
	.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
	/*
	 * Datasheet suggests max HS rate for NT35510 is 250 MHz
	 * (period time 4ns, see figure 7.6.4 page 365) and max LP rate is
	 * 20 MHz (period time 50ns, see figure 7.6.6. page 366).
	 * However these frequencies appear in source code for the Hydis
	 * HVA40WV1 panel and setting up the LP frequency makes the panel
	 * not work.
	 */
	.hs_rate = 349440000,
	.lp_rate = 9600000,
	.init_cmds = hydis_hva40wv1_init_cmds,
};

static const struct of_device_id nt35510_of_match[] = {
	{
		.compatible = "hydis,hva40wv1",
		.data = &nt35510_hydis_hva40wv1,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, nt35510_of_match);

static struct mipi_dsi_driver nt35510_driver = {
	.probe = nt35510_probe,
	.remove = nt35510_remove,
	.driver = {
		.name = "panel-novatek-nt35510",
		.of_match_table = nt35510_of_match,
	},
};
module_mipi_dsi_driver(nt35510_driver);

MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("NT35510-based panel driver");
MODULE_LICENSE("GPL v2");
