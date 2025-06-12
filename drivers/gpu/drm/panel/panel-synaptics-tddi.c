// SPDX-License-Identifier: GPL-2.0
/*
 * Synaptics TDDI display panel driver.
 *
 * Copyright (C) 2025 Kaustabh Chakraborty <kauschluss@disroot.org>
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct tddi_panel_data {
	u8 lanes;
	/* wait timings for panel enable */
	u8 delay_ms_sleep_exit;
	u8 delay_ms_display_on;
	/* wait timings for panel disable */
	u8 delay_ms_display_off;
	u8 delay_ms_sleep_enter;
};

struct tddi_ctx {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_display_mode mode;
	struct backlight_device *backlight;
	const struct tddi_panel_data *data;
	struct regulator_bulk_data supplies[3];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *backlight_gpio;
	u32 bus_flags;
	u32 width;
	u32 height;
};

static inline struct tddi_ctx *to_tddi_ctx(struct drm_panel *panel)
{
	return container_of(panel, struct tddi_ctx, panel);
}

static int tddi_update_brightness(struct backlight_device *backlight)
{
	struct mipi_dsi_multi_context dsi = { .dsi = bl_get_data(backlight) };
	u8 brightness = backlight->props.brightness;

	mipi_dsi_dcs_set_display_brightness_multi(&dsi, brightness);

	return dsi.accum_err;
}

static int tddi_prepare(struct drm_panel *panel)
{
	struct tddi_ctx *ctx = to_tddi_ctx(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);

	gpiod_set_value_cansleep(ctx->backlight_gpio, 0);
	usleep_range(5000, 6000);

	return 0;
}

static int tddi_unprepare(struct drm_panel *panel)
{
	struct tddi_ctx *ctx = to_tddi_ctx(panel);

	gpiod_set_value_cansleep(ctx->backlight_gpio, 1);
	usleep_range(5000, 6000);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);

	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static int tddi_enable(struct drm_panel *panel)
{
	struct tddi_ctx *ctx = to_tddi_ctx(panel);
	struct mipi_dsi_multi_context dsi = { .dsi = ctx->dsi };
	u8 brightness = ctx->backlight->props.brightness;

	mipi_dsi_dcs_write_seq_multi(&dsi, MIPI_DCS_WRITE_POWER_SAVE, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x0c);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi);
	mipi_dsi_msleep(&dsi, ctx->data->delay_ms_sleep_exit);

	/* sync the panel with the backlight's brightness level */
	mipi_dsi_dcs_set_display_brightness_multi(&dsi, brightness);

	mipi_dsi_dcs_set_display_on_multi(&dsi);
	mipi_dsi_msleep(&dsi, ctx->data->delay_ms_display_on);

	return dsi.accum_err;
};

static int tddi_disable(struct drm_panel *panel)
{
	struct tddi_ctx *ctx = to_tddi_ctx(panel);
	struct mipi_dsi_multi_context dsi = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi);
	mipi_dsi_msleep(&dsi, ctx->data->delay_ms_display_off);

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi);
	mipi_dsi_msleep(&dsi, ctx->data->delay_ms_sleep_enter);

	return dsi.accum_err;
}

static int tddi_get_modes(struct drm_panel *panel,
			  struct drm_connector *connector)
{
	struct tddi_ctx *ctx = to_tddi_ctx(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &ctx->mode);
	if (!mode)
		return -ENOMEM;

	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	drm_mode_set_name(mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	connector->display_info.bus_flags = ctx->bus_flags;

	return 1;
}

static const struct backlight_ops tddi_bl_ops = {
	.update_status = tddi_update_brightness,
};

static const struct drm_panel_funcs tddi_drm_panel_funcs = {
	.prepare = tddi_prepare,
	.unprepare = tddi_unprepare,
	.enable = tddi_enable,
	.disable = tddi_disable,
	.get_modes = tddi_get_modes,
};

static int tddi_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct tddi_ctx *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->data = of_device_get_match_data(dev);

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->supplies[0].supply = "vio";
	ctx->supplies[1].supply = "vsn";
	ctx->supplies[2].supply = "vsp";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->backlight_gpio = devm_gpiod_get_optional(dev, "backlight", GPIOD_ASIS);
	if (IS_ERR(ctx->backlight_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->backlight_gpio),
				     "Failed to get backlight-gpios\n");

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ret = of_get_drm_panel_display_mode(dev->of_node, &ctx->mode,
					    &ctx->bus_flags);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get panel timings\n");

	ctx->backlight = devm_backlight_device_register(dev, dev_name(dev), dev,
							dsi, &tddi_bl_ops, NULL);
	if (IS_ERR(ctx->backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->backlight),
				     "Failed to register backlight device");

	ctx->backlight->props.type = BACKLIGHT_PLATFORM;
	ctx->backlight->props.brightness = 255;
	ctx->backlight->props.max_brightness = 255;

	dsi->lanes = ctx->data->lanes;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_VIDEO_NO_HFP;

	drm_panel_init(&ctx->panel, dev, &tddi_drm_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	drm_panel_add(&ctx->panel);

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void tddi_remove(struct mipi_dsi_device *dsi)
{
	struct tddi_ctx *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct tddi_panel_data td4101_panel_data = {
	.lanes = 2,
	/* wait timings for panel enable */
	.delay_ms_sleep_exit = 100,
	.delay_ms_display_on = 0,
	/* wait timings for panel disable */
	.delay_ms_display_off = 20,
	.delay_ms_sleep_enter = 90,
};

static const struct tddi_panel_data td4300_panel_data = {
	.lanes = 4,
	/* wait timings for panel enable */
	.delay_ms_sleep_exit = 100,
	.delay_ms_display_on = 0,
	/* wait timings for panel disable */
	.delay_ms_display_off = 0,
	.delay_ms_sleep_enter = 0,
};

static const struct of_device_id tddi_of_device_id[] = {
	{
		.compatible = "syna,td4101-panel",
		.data = &td4101_panel_data,
	}, {
		.compatible = "syna,td4300-panel",
		.data = &td4300_panel_data,
	}, { }
};
MODULE_DEVICE_TABLE(of, tddi_of_device_id);

static struct mipi_dsi_driver tddi_dsi_driver = {
	.probe = tddi_probe,
	.remove = tddi_remove,
	.driver = {
		.name = "panel-synaptics-tddi",
		.of_match_table = tddi_of_device_id,
	},
};
module_mipi_dsi_driver(tddi_dsi_driver);

MODULE_AUTHOR("Kaustabh Chakraborty <kauschluss@disroot.org>");
MODULE_DESCRIPTION("Synaptics TDDI Display Panel Driver");
MODULE_LICENSE("GPL");
