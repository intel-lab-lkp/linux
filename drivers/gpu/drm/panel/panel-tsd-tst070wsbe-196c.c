// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree.
 * Copyright (c) 2026 IMD Technologies Ltd <william.bright@imd-tec.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct tst070wsbe_196c {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *supply;
	struct gpio_desc *reset_gpio;
};

static inline struct tst070wsbe_196c *to_tst070wsbe_196c(struct drm_panel *panel)
{
	return container_of_const(panel, struct tst070wsbe_196c, panel);
}

static void tst070wsbe_196c_reset(struct tst070wsbe_196c *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(200);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
}

static int tst070wsbe_196c_prepare(struct drm_panel *panel)
{
	struct tst070wsbe_196c *ctx = to_tst070wsbe_196c(panel);
	int ret;

	ret = regulator_enable(ctx->supply);
	if (ret < 0)
		return ret;

	msleep(20);

	tst070wsbe_196c_reset(ctx);

	return 0;
}

static int tst070wsbe_196c_unprepare(struct drm_panel *panel)
{
	struct tst070wsbe_196c *ctx = to_tst070wsbe_196c(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_disable(ctx->supply);

	return 0;
}

static const struct drm_display_mode tst070wsbe_196c_mode = {
	.clock = (1024 + 160 + 12 + 160) * (600 + 12 + 10 + 23) * 60 / 1000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 12,
	.htotal = 1024 + 160 + 12 + 160,
	.vdisplay = 600,
	.vsync_start = 600 + 12,
	.vsync_end = 600 + 12 + 10,
	.vtotal = 600 + 12 + 10 + 23,
	.width_mm = 190,
	.height_mm = 121,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int tst070wsbe_196c_get_modes(struct drm_panel *panel,
				   struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector,
						    &tst070wsbe_196c_mode);
}

static const struct drm_panel_funcs tst070wsbe_196c_panel_funcs = {
	.prepare = tst070wsbe_196c_prepare,
	.unprepare = tst070wsbe_196c_unprepare,
	.get_modes = tst070wsbe_196c_get_modes,
};

static int tst070wsbe_196c_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct tst070wsbe_196c *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct tst070wsbe_196c, panel,
				   &tst070wsbe_196c_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply))
		return dev_err_probe(dev, PTR_ERR(ctx->supply),
				     "Failed to get power regulator\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void tst070wsbe_196c_remove(struct mipi_dsi_device *dsi)
{
	struct tst070wsbe_196c *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id tst070wsbe_196c_of_match[] = {
	{ .compatible = "tsd,tst070wsbe-196c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tst070wsbe_196c_of_match);

static struct mipi_dsi_driver tst070wsbe_196c_driver = {
	.probe = tst070wsbe_196c_probe,
	.remove = tst070wsbe_196c_remove,
	.driver = {
		.name = "panel-tsd-tst070wsbe-196c",
		.of_match_table = tst070wsbe_196c_of_match,
	},
};
module_mipi_dsi_driver(tst070wsbe_196c_driver);

MODULE_AUTHOR("William Bright <william.bright@imd-tec.com>");
MODULE_DESCRIPTION("DRM driver for TSD TST070WSBE-196C 7\" MIPI-DSI panel");
MODULE_LICENSE("GPL");
