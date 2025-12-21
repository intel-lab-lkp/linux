// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Marijn Suijten <marijn.suijten@somainline.org>
 *
 * Based on the following Sony downstream DTS command sequences:
 * - Xperia 1 (kumano griffin): https://github.com/sonyxperiadev/kernel-copyleft/blob/55.2.A.4.xxx/arch/arm64/boot/dts/somc/dsi-panel-4koled.dtsi
 * - Xperia 1 II (edo pdx203): https://github.com/sonyxperiadev/kernel-copyleft-dts/blob/58.2.A.10.xxx/somc/dsi-panel-souxp00_a-amb650wh07-uhd.dtsi
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>

#define WRITE_CONTROL_DISPLAY_BACKLIGHT BIT(5)

/* Only used to send a few differentiating DCS between the two panel variants,
 * without exactly knowing what they mean.  These do not seem to be related to
 * panel functionality nor have any visual impact, but they are sent anyway just
 * in case.
 */
enum panel_type {
	/* Sony Xperia 1 */
	PANEL_TYPE_01,
	/* Sony Xperia 1 II */
	PANEL_TYPE_07,
};

/*
 * TODO: This should be communicated via the mode when .prepare
 * receives atomic commit info
 */
static const bool enable_4k = true;

const struct regulator_bulk_data samsung_souxp00_a_supplies[] = {
	{ .supply = "vddio", /* 1.8 V */ },
	{ .supply = "vci", /* 3.0 V */ },
};

struct samsung_souxp00_a {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	enum panel_type panel_type;
};

static inline struct samsung_souxp00_a *
to_samsung_souxp00_a(struct drm_panel *panel)
{
	return container_of(panel, struct samsung_souxp00_a, panel);
}

static int samsung_souxp00_a_program(struct samsung_souxp00_a *ctx)
{
	const u16 hdisplay = enable_4k ? 1644 : 1096;
	const u16 vdisplay = enable_4k ? 3840 : 2560;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	dsi_ctx.dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 10);

	if (ctx->panel_type == PANEL_TYPE_07) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xdd, 0x00, 0x80, 0x00, 0x00);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	}

	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	mipi_dsi_compression_mode_multi(&dsi_ctx, true);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd7, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2, enable_4k ? 0 : 1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0, hdisplay - 1);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0, vdisplay - 1);

	if (ctx->panel_type == PANEL_TYPE_01) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x70);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x00, 0x60);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	}

	/* Enable backlight control */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     WRITE_CONTROL_DISPLAY_BACKLIGHT);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x2e, 0x21);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_msleep(&dsi_ctx, 110);

	return dsi_ctx.accum_err;
}

static int samsung_souxp00_a_prepare(struct drm_panel *panel)
{
	struct samsung_souxp00_a *ctx = to_samsung_souxp00_a(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	struct drm_dsc_picture_parameter_set pps;
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(samsung_souxp00_a_supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);

	ret = samsung_souxp00_a_program(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to program panel: %d\n", ret);
		goto fail;
	}

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	mipi_dsi_compression_mode_multi(&dsi_ctx, true);
	mipi_dsi_msleep(&dsi_ctx, 28);

	ret = dsi_ctx.accum_err;

	if (ret < 0)
		goto fail;

	return 0;

fail:
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(samsung_souxp00_a_supplies), ctx->supplies);
	return ret;
}

static int samsung_souxp00_a_enable(struct drm_panel *panel)
{
	struct samsung_souxp00_a *ctx = to_samsung_souxp00_a(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int samsung_souxp00_a_disable(struct drm_panel *panel)
{
	struct samsung_souxp00_a *ctx = to_samsung_souxp00_a(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 17);

	return dsi_ctx.accum_err;
}

static int samsung_souxp00_a_unprepare(struct drm_panel *panel)
{
	struct samsung_souxp00_a *ctx = to_samsung_souxp00_a(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	if (ctx->panel_type == PANEL_TYPE_01) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					     WRITE_CONTROL_DISPLAY_BACKLIGHT);
		mipi_dsi_msleep(&dsi_ctx, 17);
	}

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	dsi_ctx.dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(samsung_souxp00_a_supplies), ctx->supplies);

	return dsi_ctx.accum_err;
}

/*
 * Small fake porch to force the DSI pclk/byteclk
 * high enough to have a smooth panel at 60Hz.
 *
 * 60 for 4k, or 40 for 2.5k is fine for Griffin, but not PDX203.
 * Even at 84 there's still the occasional hiccup.
 */
static const int fake_porch = 98;

static const struct drm_display_mode samsung_souxp00_a_2_5k_mode = {
	.clock = (1096 + fake_porch) * 2560 * 60 / 1000,
	.hdisplay = 1096,
	.hsync_start = 1096 + fake_porch,
	.hsync_end = 1096 + fake_porch,
	.htotal = 1096 + fake_porch,
	.vdisplay = 2560,
	.vsync_start = 2560,
	.vsync_end = 2560,
	.vtotal = 2560,
	.width_mm = 65,
	.height_mm = 152,
	.type = DRM_MODE_TYPE_DRIVER,
};

static const struct drm_display_mode samsung_souxp00_a_4k_mode = {
	.clock = (1644 + fake_porch) * 3840 * 60 / 1000,
	.hdisplay = 1644,
	.hsync_start = 1644 + fake_porch,
	.hsync_end = 1644 + fake_porch,
	.htotal = 1644 + fake_porch,
	.vdisplay = 3840,
	.vsync_start = 3840,
	.vsync_end = 3840,
	.vtotal = 3840,
	.width_mm = 65,
	.height_mm = 152,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int samsung_souxp00_a_get_modes(struct drm_panel *panel,
				     struct drm_connector *connector)
{
	if (enable_4k)
		return drm_connector_helper_get_modes_fixed(
			connector, &samsung_souxp00_a_4k_mode);
	else
		return drm_connector_helper_get_modes_fixed(
			connector, &samsung_souxp00_a_2_5k_mode);
}

static const struct drm_panel_funcs samsung_souxp00_a_panel_funcs = {
	.prepare = samsung_souxp00_a_prepare,
	.enable = samsung_souxp00_a_enable,
	.disable = samsung_souxp00_a_disable,
	.unprepare = samsung_souxp00_a_unprepare,
	.get_modes = samsung_souxp00_a_get_modes,
};

static int samsung_souxp00_a_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);

	return mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
}

static int samsung_souxp00_a_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);

	if (ret < 0)
		return ret;

	return brightness;
}

static const struct backlight_ops samsung_souxp00_a_bl_ops = {
	.update_status = samsung_souxp00_a_bl_update_status,
	.get_brightness = samsung_souxp00_a_bl_get_brightness,
};

static int samsung_souxp00_a_probe(struct mipi_dsi_device *dsi)
{
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 400,
		.max_brightness = 4095,
	};
	const u16 hdisplay = enable_4k ? 1644 : 1096;
	struct device *dev = &dsi->dev;
	struct samsung_souxp00_a *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct samsung_souxp00_a, panel,
				   &samsung_souxp00_a_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->panel_type = (enum panel_type)of_device_get_match_data(dev);

	ret = devm_regulator_bulk_get_const(
		dev,
		ARRAY_SIZE(samsung_souxp00_a_supplies),
		samsung_souxp00_a_supplies,
		&ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = devm_backlight_device_register(
		dev, dev_name(dev), dev, dsi,
		&samsung_souxp00_a_bl_ops,
		&props);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	ctx->dsc.slice_height = 32;
	ctx->dsc.slice_count = 2;
	/*
	 * hdisplay should be read from the selected mode once
	 * it is passed back to drm_panel (in prepare?)
	 */
	WARN_ON(hdisplay % ctx->dsc.slice_count);
	ctx->dsc.slice_width = hdisplay / ctx->dsc.slice_count;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void samsung_souxp00_a_remove(struct mipi_dsi_device *dsi)
{
	struct samsung_souxp00_a *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id samsung_souxp00_a_of_match[] = {
	{ .compatible = "samsung,souxp00-a-amb650wh01",
	  .data = (void *)PANEL_TYPE_01 },
	{ .compatible = "samsung,souxp00-a-amb650wh07",
	  .data = (void *)PANEL_TYPE_07 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, samsung_souxp00_a_of_match);

static struct mipi_dsi_driver samsung_souxp00_a_driver = {
	.probe = samsung_souxp00_a_probe,
	.remove = samsung_souxp00_a_remove,
	.driver = {
		.name = "panel-samsung-souxp00-a",
		.of_match_table = samsung_souxp00_a_of_match,
	},
};
module_mipi_dsi_driver(samsung_souxp00_a_driver);

MODULE_AUTHOR("Marijn Suijten <marijn.suijten@somainline.org>");
MODULE_DESCRIPTION("DRM panel driver for Samsung SOUXP00-A Display-Driver-IC panels");
MODULE_LICENSE("GPL");
