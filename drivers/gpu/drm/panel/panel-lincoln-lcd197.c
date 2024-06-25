// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 BayLibre, SAS
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct lincoln_lcd197_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *supply;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;
};

static inline
struct lincoln_lcd197_panel *to_lincoln_lcd197_panel(struct drm_panel *panel)
{
	return container_of(panel, struct lincoln_lcd197_panel, panel);
}

static int lincoln_lcd197_panel_prepare(struct drm_panel *panel)
{
	struct lincoln_lcd197_panel *lcd = to_lincoln_lcd197_panel(panel);
	int err;
	u8 display_id[3];

	if (lcd->prepared)
		return 0;

	gpiod_set_value_cansleep(lcd->enable_gpio, 0);
	err = regulator_enable(lcd->supply);
	if (err < 0)
		return err;

	gpiod_set_value_cansleep(lcd->enable_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(lcd->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(lcd->reset_gpio, 0);
	msleep(50);

	mipi_dsi_dcs_write_seq(lcd->dsi, 0xB9, 0xFF, 0x83, 0x99);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xD2, 0x55);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xB1, 0x02, 0x04, 0x70, 0x90, 0x01,
			       0x32, 0x33, 0x11, 0x11, 0x4D, 0x57, 0x56, 0x73,
			       0x02, 0x02);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xB2, 0x00, 0x80, 0x80, 0xAE, 0x0A,
			       0x0E, 0x75, 0x11, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xB4, 0x00, 0xFF, 0x04, 0xA4, 0x02,
			       0xA0, 0x00, 0x00, 0x10, 0x00, 0x00, 0x02, 0x00,
			       0x24, 0x02, 0x04, 0x0A, 0x21, 0x03, 0x00, 0x00,
			       0x08, 0xA6, 0x88, 0x04, 0xA4, 0x02, 0xA0, 0x00,
			       0x00, 0x10, 0x00, 0x00, 0x02, 0x00, 0x24, 0x02,
			       0x04, 0x0A, 0x00, 0x00, 0x08, 0xA6, 0x00, 0x08,
			       0x11);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xD3, 0x00, 0x00, 0x00, 0X00, 0x00,
			       0x00, 0x18, 0x18, 0x32, 0x10, 0x09, 0x00, 0x09,
			       0x32, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x11, 0x00, 0x02, 0x02, 0x03, 0x00,
			       0x00, 0x00, 0x0A, 0x40);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xD5, 0x18, 0x18, 0x18, 0x18, 0x21,
			       0x20, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19, 0x18,
			       0x18, 0x18, 0x18, 0x03, 0x02, 0x01, 0x00, 0x2F,
			       0x2F, 0x30, 0x30, 0x31, 0x31, 0x18, 0x18, 0x18,
			       0x18, 0x18, 0x18);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xD6, 0x18, 0x18, 0x18, 0x18, 0x20,
			       0x21, 0x19, 0x19, 0x18, 0x18, 0x19, 0x19, 0x18,
			       0x18, 0x18, 0x18, 0x00, 0x01, 0x02, 0x03, 0x2F,
			       0x2F, 0x30, 0x30, 0x31, 0x31, 0x18, 0x18, 0x18,
			       0x18, 0x18, 0x18);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xBD, 0x01);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xD8, 0x0A, 0xBE, 0xFA, 0xA0, 0x0A,
			       0xBE, 0xFA, 0xA0);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xD8, 0x0F, 0xFF, 0xFF, 0xE0, 0x0F,
			       0xFF, 0xFF, 0xE0);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xBD, 0x02);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xD8, 0x0F, 0xFF, 0xFF, 0xE0, 0x0F,
			       0xFF, 0xFF, 0xE0);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xE0, 0x01, 0x11, 0x1C, 0x17, 0x39,
			       0x43, 0x54, 0x51, 0x5A, 0x64, 0x6C, 0x74, 0x7A,
			       0x83, 0x8D, 0x92, 0x99, 0xA4, 0xA9, 0xB4, 0xAA,
			       0xBA, 0xBE, 0x63, 0x5E, 0x69, 0x73, 0x01, 0x11,
			       0x1C, 0x17, 0x39, 0x43, 0x54, 0x51, 0x5A, 0x64,
			       0x6C, 0x74, 0x7A, 0x83, 0x8D, 0x92, 0x99, 0xA4,
			       0xA7, 0xB2, 0xA9, 0xBA, 0xBE, 0x63, 0x5E, 0x69,
			       0x73);
	usleep_range(200, 300);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xB6, 0x92, 0x92);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xCC, 0x00);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xBF, 0x40, 0x41, 0x50, 0x49);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xC6, 0xFF, 0xF9);
	mipi_dsi_dcs_write_seq(lcd->dsi, 0xC0, 0x25, 0x5A);
	mipi_dsi_dcs_write_seq(lcd->dsi, MIPI_DCS_SET_ADDRESS_MODE, 0x02);

	err = mipi_dsi_dcs_exit_sleep_mode(lcd->dsi);
	if (err < 0) {
		dev_err(panel->dev, "failed to exit sleep mode: %d\n", err);
		goto poweroff;
	}
	msleep(120);

	err = mipi_dsi_dcs_read(lcd->dsi, MIPI_DCS_GET_DISPLAY_ID, display_id, 3);
	if (err < 0) {
		dev_err(panel->dev, "Failed to read display id: %d\n", err);
	} else {
		dev_dbg(panel->dev, "Display id: 0x%02x-0x%02x-0x%02x\n",
			display_id[0], display_id[1], display_id[2]);
	}

	lcd->prepared = true;

	return 0;

poweroff:
	gpiod_set_value_cansleep(lcd->enable_gpio, 0);
	gpiod_set_value_cansleep(lcd->reset_gpio, 1);
	regulator_disable(lcd->supply);

	return err;
}

static int lincoln_lcd197_panel_unprepare(struct drm_panel *panel)
{
	struct lincoln_lcd197_panel *lcd = to_lincoln_lcd197_panel(panel);
	int err;

	if (!lcd->prepared)
		return 0;

	lcd->prepared = false;

	err = mipi_dsi_dcs_enter_sleep_mode(lcd->dsi);
	if (err < 0)
		dev_err(panel->dev, "failed to enter sleep mode: %d\n", err);

	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(lcd->enable_gpio, 0);
	gpiod_set_value_cansleep(lcd->reset_gpio, 1);
	regulator_disable(lcd->supply);

	return err;
}

static int lincoln_lcd197_panel_enable(struct drm_panel *panel)
{
	struct lincoln_lcd197_panel *lcd = to_lincoln_lcd197_panel(panel);
	int err;

	if (lcd->enabled)
		return 0;

	err = mipi_dsi_dcs_set_display_on(lcd->dsi);
	if (err < 0) {
		dev_err(panel->dev, "failed to set display on: %d\n", err);
		return err;
	}

	msleep(20);
	lcd->enabled = true;

	return 0;
}

static int lincoln_lcd197_panel_disable(struct drm_panel *panel)
{
	struct lincoln_lcd197_panel *lcd = to_lincoln_lcd197_panel(panel);
	int err;

	if (!lcd->enabled)
		return 0;

	err = mipi_dsi_dcs_set_display_off(lcd->dsi);
	if (err < 0)
		dev_err(panel->dev, "failed to set display off: %d\n", err);


	msleep(50);
	lcd->enabled = false;

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = 154002,
	.hdisplay = 1080,
	.hsync_start = 1080 + 20,
	.hsync_end = 1080 + 20 + 6,
	.htotal = 1080 + 204,
	.vdisplay = 1920,
	.vsync_start = 1920 + 4,
	.vsync_end = 1920 + 4 + 4,
	.vtotal = 1920 + 79,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static int lincoln_lcd197_panel_get_modes(struct drm_panel *panel,
					  struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = 79;
	connector->display_info.height_mm = 125;

	return 1;
}

static const struct drm_panel_funcs lincoln_lcd197_panel_funcs = {
	.prepare = lincoln_lcd197_panel_prepare,
	.unprepare = lincoln_lcd197_panel_unprepare,
	.enable = lincoln_lcd197_panel_enable,
	.disable = lincoln_lcd197_panel_disable,
	.get_modes = lincoln_lcd197_panel_get_modes,
};

static int lincoln_lcd197_panel_probe(struct mipi_dsi_device *dsi)
{
	struct lincoln_lcd197_panel *lcd;
	struct device *dev = &dsi->dev;
	int err;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = (MIPI_DSI_MODE_VIDEO |
			   MIPI_DSI_MODE_VIDEO_BURST |
			   MIPI_DSI_MODE_LPM);

	lcd = devm_kzalloc(&dsi->dev, sizeof(*lcd), GFP_KERNEL);
	if (!lcd)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, lcd);
	lcd->dsi = dsi;

	lcd->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(lcd->supply))
		return dev_err_probe(dev, PTR_ERR(lcd->supply),
				     "failed to get power supply");

	lcd->enable_gpio = devm_gpiod_get(dev, "enable",
					  GPIOD_OUT_HIGH);
	if (IS_ERR(lcd->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(lcd->enable_gpio),
				     "failed to get enable gpio");

	lcd->reset_gpio = devm_gpiod_get(dev, "reset",
					  GPIOD_OUT_HIGH);
	if (IS_ERR(lcd->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(lcd->reset_gpio),
				     "failed to get reset gpio");

	drm_panel_init(&lcd->panel, dev,
		       &lincoln_lcd197_panel_funcs, DRM_MODE_CONNECTOR_DSI);

	err = drm_panel_of_backlight(&lcd->panel);
	if (err)
		return err;

	drm_panel_add(&lcd->panel);
	err = mipi_dsi_attach(dsi);
	if (err)
		drm_panel_remove(&lcd->panel);

	return err;
}

static void lincoln_lcd197_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct lincoln_lcd197_panel *lcd = mipi_dsi_get_drvdata(dsi);

	drm_panel_disable(&lcd->panel);
	drm_panel_unprepare(&lcd->panel);
}

static void lincoln_lcd197_panel_remove(struct mipi_dsi_device *dsi)
{
	struct lincoln_lcd197_panel *lcd = mipi_dsi_get_drvdata(dsi);
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	lincoln_lcd197_panel_shutdown(dsi);
	drm_panel_remove(&lcd->panel);
}

static const struct of_device_id lincoln_lcd197_of_match[] = {
	{ .compatible = "lincoln,lcd197", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lincoln_lcd197_of_match);

static struct mipi_dsi_driver lincoln_lcd197_panel_driver = {
	.driver = {
		.name = "panel-licoln-lcd197",
		.of_match_table = lincoln_lcd197_of_match,
	},
	.probe = lincoln_lcd197_panel_probe,
	.remove = lincoln_lcd197_panel_remove,
	.shutdown = lincoln_lcd197_panel_shutdown,
};
module_mipi_dsi_driver(lincoln_lcd197_panel_driver);

MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_DESCRIPTION("Lincoln LCD197 panel driver");
MODULE_LICENSE("GPL");
