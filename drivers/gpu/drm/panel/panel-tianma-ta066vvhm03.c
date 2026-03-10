// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree.
 * Copyright (c) 2026 Alexander Koskovich <akoskovich@pm.me>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct tianma_ta066vvhm03 {
	struct regulator_bulk_data *supplies;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct drm_panel panel;
};

static const struct regulator_bulk_data tianma_ta066vvhm03_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vci" },
	{ .supply = "vdd" },
};

static inline
struct tianma_ta066vvhm03 *to_tianma_ta066vvhm03(struct drm_panel *panel)
{
	return container_of(panel, struct tianma_ta066vvhm03, panel);
}

static void tianma_ta066vvhm03_reset(struct tianma_ta066vvhm03 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int tianma_ta066vvhm03_on(struct tianma_ta066vvhm03 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb3, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0x2a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc1, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc1,
				     0x94, 0x42, 0x00, 0x16, 0x05, 0x00, 0x00,
				     0x00, 0x10, 0x00, 0x10, 0x00, 0xaa, 0x8a,
				     0x02, 0x10, 0x00, 0x10, 0x00, 0x00, 0x3f,
				     0x3f, 0x03, 0xff, 0x03, 0xff, 0x23, 0xff,
				     0x03, 0xff, 0x23, 0xff, 0x03, 0xff, 0x00,
				     0x40, 0x40, 0x00, 0x00, 0x10, 0x01, 0x00,
				     0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc2,
				     0x09, 0x24, 0x0e, 0x00, 0x00, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc4,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
				     0x00, 0x2f, 0x00, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcf,
				     0x64, 0x0b, 0x00, 0xc0, 0x02, 0xa6, 0x04,
				     0x7f, 0x0b, 0x77, 0x0b, 0x8b, 0x04, 0x04,
				     0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
				     0x05, 0x05, 0x05, 0x00, 0x10, 0x01, 0x68,
				     0x01, 0x68, 0x01, 0x68, 0x01, 0x68, 0x01,
				     0x68, 0x01, 0x69, 0x03, 0x98, 0x03, 0x70,
				     0x03, 0x70, 0x03, 0x70, 0x03, 0x70, 0x00,
				     0x10, 0x01, 0x68, 0x01, 0x68, 0x01, 0x68,
				     0x01, 0x68, 0x01, 0x68, 0x01, 0x68, 0x03,
				     0x98, 0x03, 0x70, 0x03, 0x70, 0x03, 0x70,
				     0x03, 0x70, 0x01, 0x42, 0x01, 0x42, 0x01,
				     0x42, 0x01, 0x42, 0x01, 0x42, 0x01, 0x42,
				     0x01, 0x42, 0x01, 0x42, 0x01, 0x42, 0x01,
				     0x42, 0x01, 0x42, 0x01, 0x42, 0x1c, 0x1c,
				     0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x1c,
				     0x1c, 0x1c, 0x00, 0x01, 0x9a, 0x01, 0x9a,
				     0x01, 0x9a, 0x05, 0xae, 0x05, 0xae, 0x09,
				     0xa4, 0x09, 0xa4, 0x09, 0xa4, 0x09, 0xa4,
				     0x09, 0xa4, 0x09, 0xa4, 0x0f, 0xc3, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd7,
				     0x00, 0xa9, 0x34, 0x00, 0x20, 0x02, 0x00,
				     0x00, 0x30, 0x00, 0x40, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x10, 0x02, 0x00, 0x40,
				     0x09, 0x00, 0x00, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd8,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x30, 0x00, 0x30, 0x00, 0x30,
				     0x00, 0x30, 0x00, 0x30, 0x05, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x0f, 0x00, 0x2f, 0x00, 0x00,
				     0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbb,
				     0x59, 0xc8, 0xc8, 0xc8, 0xc8, 0xc8, 0xc8,
				     0xc8, 0xc8, 0xc8, 0x4a, 0x48, 0x46, 0x44,
				     0x42, 0x40, 0x3e, 0x3c, 0x3a, 0x00, 0xff,
				     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
				     0xff, 0x04, 0x00, 0x04, 0x04, 0x42, 0x04,
				     0x69, 0x5a, 0x00, 0x0a, 0xb0, 0x0f, 0xff,
				     0x0f, 0xff, 0x0f, 0xff, 0x14, 0x81, 0xf4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe8, 0x00, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4, 0x00, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd4, 0x93);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xde, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xdf, 0x50, 0x40);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf3,
				     0x50, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x11);
	mipi_dsi_usleep_range(&dsi_ctx, 1000, 2000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf3,
				     0x01, 0x00, 0x00, 0x00, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x00, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x19);
	mipi_dsi_usleep_range(&dsi_ctx, 1000, 2000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xdf, 0x50, 0x42);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x24);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0x0000, 0x0437);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0x0000, 0x0923);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe6, 0x01);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int tianma_ta066vvhm03_off(struct tianma_ta066vvhm03 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int tianma_ta066vvhm03_prepare(struct drm_panel *panel)
{
	struct tianma_ta066vvhm03 *ctx = to_tianma_ta066vvhm03(panel);
	struct drm_dsc_picture_parameter_set pps;
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(tianma_ta066vvhm03_supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(ctx->enable_gpio, 1);

	tianma_ta066vvhm03_reset(ctx);

	ret = tianma_ta066vvhm03_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		gpiod_set_value_cansleep(ctx->enable_gpio, 0);
		regulator_bulk_disable(ARRAY_SIZE(tianma_ta066vvhm03_supplies), ctx->supplies);
		return ret;
	}

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "failed to transmit PPS: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		return ret;
	}

	return 0;
}

static int tianma_ta066vvhm03_unprepare(struct drm_panel *panel)
{
	struct tianma_ta066vvhm03 *ctx = to_tianma_ta066vvhm03(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = tianma_ta066vvhm03_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(tianma_ta066vvhm03_supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode tianma_ta066vvhm03_mode = {
	.clock = (1080 + 24 + 4 + 10) * (2340 + 12 + 1 + 4) * 160 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 24,
	.hsync_end = 1080 + 24 + 4,
	.htotal = 1080 + 24 + 4 + 10,
	.vdisplay = 2340,
	.vsync_start = 2340 + 12,
	.vsync_end = 2340 + 12 + 1,
	.vtotal = 2340 + 12 + 1 + 4,
	.width_mm = 70,
	.height_mm = 152,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int tianma_ta066vvhm03_get_modes(struct drm_panel *panel,
					struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &tianma_ta066vvhm03_mode);
}

static const struct drm_panel_funcs tianma_ta066vvhm03_panel_funcs = {
	.prepare = tianma_ta066vvhm03_prepare,
	.unprepare = tianma_ta066vvhm03_unprepare,
	.get_modes = tianma_ta066vvhm03_get_modes,
};

static int tianma_ta066vvhm03_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static const struct backlight_ops tianma_ta066vvhm03_bl_ops = {
	.update_status = tianma_ta066vvhm03_bl_update_status,
};

static struct backlight_device *
tianma_ta066vvhm03_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 4095,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &tianma_ta066vvhm03_bl_ops, &props);
}

static int tianma_ta066vvhm03_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct tianma_ta066vvhm03 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct tianma_ta066vvhm03, panel,
				   &tianma_ta066vvhm03_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(tianma_ta066vvhm03_supplies),
					    tianma_ta066vvhm03_supplies,
					    &ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "Failed to get enable-gpios\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = tianma_ta066vvhm03_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;
	dsi->dsc_slice_per_pkt = 2;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	ctx->dsc.slice_height = 20;
	ctx->dsc.slice_width = 540;
	WARN_ON(1080 % ctx->dsc.slice_width);
	ctx->dsc.slice_count = 1080 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void tianma_ta066vvhm03_remove(struct mipi_dsi_device *dsi)
{
	struct tianma_ta066vvhm03 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id tianma_ta066vvhm03_of_match[] = {
	{ .compatible = "tianma,ta066vvhm03" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tianma_ta066vvhm03_of_match);

static struct mipi_dsi_driver tianma_ta066vvhm03_driver = {
	.probe = tianma_ta066vvhm03_probe,
	.remove = tianma_ta066vvhm03_remove,
	.driver = {
		.name = "panel-tianma-ta066vvhm03",
		.of_match_table = tianma_ta066vvhm03_of_match,
	},
};
module_mipi_dsi_driver(tianma_ta066vvhm03_driver);

MODULE_AUTHOR("Alexander Koskovich <akoskovich@pm.me>");
MODULE_DESCRIPTION("DRM driver for Tianma TA066VVHM03-00");
MODULE_LICENSE("GPL");
