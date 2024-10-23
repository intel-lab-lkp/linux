// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree.
 * Copyright (c) 2024 Luca Weiss <luca.weiss@fairphone.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

/* Manufacturer specific DSI commands */
#define EK79007AD3_GAMMA1		0x80
#define EK79007AD3_GAMMA2		0x81
#define EK79007AD3_GAMMA3		0x82
#define EK79007AD3_GAMMA4		0x83
#define EK79007AD3_GAMMA5		0x84
#define EK79007AD3_GAMMA6		0x85
#define EK79007AD3_GAMMA7		0x86
#define EK79007AD3_PANEL_CTRL3		0xB2

struct ek79007ad3_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *supply;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *standby_gpio;
};

static inline struct ek79007ad3_panel *to_ek79007ad3_panel(struct drm_panel *panel)
{
	return container_of(panel, struct ek79007ad3_panel, panel);
}

static void ek79007ad3_reset(struct ek79007ad3_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(30);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(55);
}

static int ek79007ad3_on(struct ek79007ad3_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/* Gamma 2.2 */
	mipi_dsi_dcs_write_seq(dsi, EK79007AD3_GAMMA1, 0x48);
	mipi_dsi_dcs_write_seq(dsi, EK79007AD3_GAMMA2, 0xB8);
	mipi_dsi_dcs_write_seq(dsi, EK79007AD3_GAMMA3, 0x88);
	mipi_dsi_dcs_write_seq(dsi, EK79007AD3_GAMMA4, 0x88);
	mipi_dsi_dcs_write_seq(dsi, EK79007AD3_GAMMA5, 0x58);
	mipi_dsi_dcs_write_seq(dsi, EK79007AD3_GAMMA6, 0xD2);
	mipi_dsi_dcs_write_seq(dsi, EK79007AD3_GAMMA7, 0x88);
	msleep(50);

	/* 4 Lanes */
	ret = mipi_dsi_generic_write(dsi, (u8[]){ EK79007AD3_PANEL_CTRL3, 0x70 }, 2);
	if (ret)
		goto fail;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(120);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	msleep(120);

fail:
	return ret;
}

static int ek79007ad3_disable(struct drm_panel *panel)
{
	struct ek79007ad3_panel *ctx = to_ek79007ad3_panel(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(120);

	gpiod_set_value_cansleep(ctx->standby_gpio, 1);

	return 0;
}

static int ek79007ad3_prepare(struct drm_panel *panel)
{
	struct ek79007ad3_panel *ctx = to_ek79007ad3_panel(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(ctx->standby_gpio, 0);
	msleep(20);

	mipi_dsi_dcs_nop(dsi);
	usleep_range(1000, 2000);

	ek79007ad3_reset(ctx);

	ret = ek79007ad3_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_disable(ctx->supply);
		return ret;
	}

	return 0;
}

static int ek79007ad3_unprepare(struct drm_panel *panel)
{
	struct ek79007ad3_panel *ctx = to_ek79007ad3_panel(panel);

	gpiod_set_value_cansleep(ctx->standby_gpio, 1);
	msleep(50);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_disable(ctx->supply);

	return 0;
}

static const struct drm_display_mode ek79007ad3_mode = {
	.clock = 51200,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 160,
	.htotal = 1024 + 160 + 160 + 10,
	.vdisplay = 600,
	.vsync_start = 600 + 12,
	.vsync_end = 600 + 12 + 23,
	.vtotal = 600 + 23 + 12 + 1,
	.width_mm = 154,
	.height_mm = 86,
};

static int ek79007ad3_get_modes(struct drm_panel *panel,
				  struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &ek79007ad3_mode);
}

static const struct drm_panel_funcs ek79007ad3_panel_funcs = {
	.prepare = ek79007ad3_prepare,
	.unprepare = ek79007ad3_unprepare,
	.disable = ek79007ad3_disable,
	.get_modes = ek79007ad3_get_modes,
};

static int ek79007ad3_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ek79007ad3_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ctx->supply))
		return dev_err_probe(dev, ret, "Failed to get regulator\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->standby_gpio = devm_gpiod_get(dev, "standby", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->standby_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->standby_gpio),
				     "Failed to get standby-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;

	drm_panel_init(&ctx->panel, dev, &ek79007ad3_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void ek79007ad3_remove(struct mipi_dsi_device *dsi)
{
	struct ek79007ad3_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ek79007ad3_of_match[] = {
	{ .compatible = "lxd,m9189a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ek79007ad3_of_match);

static struct mipi_dsi_driver ek79007ad3_driver = {
	.probe = ek79007ad3_probe,
	.remove = ek79007ad3_remove,
	.driver = {
		.name = "panel-fitipower-ek79007ad3",
		.of_match_table = ek79007ad3_of_match,
	},
};
module_mipi_dsi_driver(ek79007ad3_driver);

MODULE_DESCRIPTION("DRM driver for ek79007ad3-equipped DSI panels");
MODULE_LICENSE("GPL");
