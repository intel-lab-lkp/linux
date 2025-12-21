// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Marijn Suijten <marijn.suijten@somainline.org>
 *
 * Based on the following Sony downstream DTS command sequence:
 * https://github.com/sonyxperiadev/kernel-copyleft/blob/52.0.A.3.xxx/arch/arm64/boot/dts/somc/dsi-panel-akatsuki_vendor.dtsi
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>

#define WRITE_CONTROL_DISPLAY_BACKLIGHT BIT(5)

const struct regulator_bulk_data lgd_lh599qh3_edb1_supplies[] = {
	{ .supply = "vddio", /* 1.8 V */ },
	{ .supply = "avdd", /* 3.0 V */ },
};

struct lgd_lh599qh3_edb1 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
};

static inline struct lgd_lh599qh3_edb1 *
to_lgd_lh599qh3_edb1(struct drm_panel *panel)
{
	return container_of(panel, struct lgd_lh599qh3_edb1, panel);
}

static int lgd_lh599qh3_edb1_program(struct lgd_lh599qh3_edb1 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	dsi_ctx.dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x7f, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x02, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x59, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     WRITE_CONTROL_DISPLAY_BACKLIGHT);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x57, 0x20, 0x80, 0xde, 0x60, 0x00);

	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0, 1440 - 1);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0, 2880 - 1);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x00);

	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x7f, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf6, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf6, 0x01, 0x7f, 0x00);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3, 0xac, 0x19, 0x34, 0x14, 0x7d);

	return 0;
}

static int lgd_lh599qh3_edb1_prepare(struct drm_panel *panel)
{
	struct lgd_lh599qh3_edb1 *ctx = to_lgd_lh599qh3_edb1(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	struct drm_dsc_picture_parameter_set pps;
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(lgd_lh599qh3_edb1_supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	msleep(100);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 5100);

	ret = lgd_lh599qh3_edb1_program(ctx);
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
	regulator_bulk_disable(ARRAY_SIZE(lgd_lh599qh3_edb1_supplies), ctx->supplies);
	return ret;
}

static int lgd_lh599qh3_edb1_enable(struct drm_panel *panel)
{
	struct lgd_lh599qh3_edb1 *ctx = to_lgd_lh599qh3_edb1(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int lgd_lh599qh3_edb1_disable(struct drm_panel *panel)
{
	struct lgd_lh599qh3_edb1 *ctx = to_lgd_lh599qh3_edb1(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	return dsi_ctx.accum_err;
}

static int lgd_lh599qh3_edb1_unprepare(struct drm_panel *panel)
{
	struct lgd_lh599qh3_edb1 *ctx = to_lgd_lh599qh3_edb1(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_tear_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	dsi_ctx.dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(lgd_lh599qh3_edb1_supplies), ctx->supplies);

	usleep_range(5000, 5100);

	return dsi_ctx.accum_err;
}

/*
 * Small fake porch to force the DSI pclk/byteclk
 * high enough to have a smooth panel at 60Hz.
 */
static const int fake_porch = 60;

static const struct drm_display_mode lgd_lh599qh3_edb1_mode = {
	.clock = (1440 + fake_porch) * 2880 * 60 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + fake_porch,
	.hsync_end = 1440 + fake_porch,
	.htotal = 1440 + fake_porch,
	.vdisplay = 2880,
	.vsync_start = 2880,
	.vsync_end = 2880,
	.vtotal = 2880,
	.width_mm = 68,
	.height_mm = 136,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int lgd_lh599qh3_edb1_get_modes(struct drm_panel *panel,
				       struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector,
						    &lgd_lh599qh3_edb1_mode);
}

static const struct drm_panel_funcs lgd_lh599qh3_edb1_panel_funcs = {
	.prepare = lgd_lh599qh3_edb1_prepare,
	.enable = lgd_lh599qh3_edb1_enable,
	.disable = lgd_lh599qh3_edb1_disable,
	.unprepare = lgd_lh599qh3_edb1_unprepare,
	.get_modes = lgd_lh599qh3_edb1_get_modes,
};

static int lgd_lh599qh3_edb1_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);

	return mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
}

static int lgd_lh599qh3_edb1_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	return brightness & 0x3ff;
}

static const struct backlight_ops lgd_lh599qh3_edb1_bl_ops = {
	.update_status = lgd_lh599qh3_edb1_bl_update_status,
	.get_brightness = lgd_lh599qh3_edb1_bl_get_brightness,
};

static int lgd_lh599qh3_edb1_probe(struct mipi_dsi_device *dsi)
{
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 100,
		.max_brightness = 1023,
	};
	struct device *dev = &dsi->dev;
	struct lgd_lh599qh3_edb1 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct lgd_lh599qh3_edb1, panel,
				   &lgd_lh599qh3_edb1_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(
		dev,
		ARRAY_SIZE(lgd_lh599qh3_edb1_supplies),
		lgd_lh599qh3_edb1_supplies,
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
		&lgd_lh599qh3_edb1_bl_ops,
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
	WARN_ON(1440 % ctx->dsc.slice_count);
	ctx->dsc.slice_width = 1440 / ctx->dsc.slice_count;
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

static void lgd_lh599qh3_edb1_remove(struct mipi_dsi_device *dsi)
{
	struct lgd_lh599qh3_edb1 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id lgd_lh599qh3_edb1_of_match[] = {
	{ .compatible = "lgd,lh599qh3-edb1-um1" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lgd_lh599qh3_edb1_of_match);

static struct mipi_dsi_driver lgd_lh599qh3_edb1_driver = {
	.probe = lgd_lh599qh3_edb1_probe,
	.remove = lgd_lh599qh3_edb1_remove,
	.driver = {
		.name = "panel-lgd-lh599qh3-edb1",
		.of_match_table = lgd_lh599qh3_edb1_of_match,
	},
};
module_mipi_dsi_driver(lgd_lh599qh3_edb1_driver);

MODULE_AUTHOR("Marijn Suijten <marijn.suijten@somainline.org>");
MODULE_DESCRIPTION("DRM panel driver for an LGD LH599QH3-EDB1 OLED assembly found in the Sony Xperia XZ3");
MODULE_LICENSE("GPL");
