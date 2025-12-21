// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Marijn Suijten <marijn.suijten@somainline.org>
 *
 * Based on the following Sony downstream DTS command sequences:
 * - Xperia 5 (kumano bahamut): https://github.com/sonyxperiadev/kernel-copyleft/blob/55.2.A.4.xxx/arch/arm64/boot/dts/somc/dsi-panel-sofef01_m-fhd_plus.dtsi
 * - Xperia 10 II (seine pdx201): https://github.com/sonyxperiadev/kernel-copyleft/blob/59.1.A.2.xxx/arch/arm64/boot/dts/somc/dsi-panel-sofef01_m-fhd_plus.dtsi
 * - Xperia 10 III (lena pdx213): https://github.com/sonyxperiadev/kernel-copyleft-dts/blob/62.1.A.0.xxx/qcom/dsi-panel-pdx213-amoled-fhd-cmd.dtsi
 * - Xperia 10 IV (murray pdx225): https://github.com/sonyxperiadev/kernel-copyleft-dts/blob/65.1.A.4.xxx/qcom/dsi-panel-samsung-amoled-fhd-cmd.dtsi
 * - Xperia 10 V (zambezi pdx235): https://github.com/sonyxperiadev/kernel-copyleft-dts/blob/68.2.A.2.xxx/qcom/dsi-panel-samsung-amoled-fhd-cmd.dtsi
 * - Xperia 10 VI (columbia pdx246): https://github.com/sonyxperiadev/kernel-copyleft-dts/blob/70.0.A.2.xxx/qcom/dsi-panel-samsung-amoled-fhd-cmd.dtsi
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

#define WRITE_CONTROL_DISPLAY_AOD_LOW BIT(0)
#define WRITE_CONTROL_DISPLAY_AOD_ON BIT(1)
#define WRITE_CONTROL_DISPLAY_DIMMING BIT(3)
#define WRITE_CONTROL_DISPLAY_LOCAL_HBM BIT(4)
#define WRITE_CONTROL_DISPLAY_BACKLIGHT BIT(5)
#define WRITE_CONTROL_DISPLAY_HBM GENMASK(6, 7)

/* Only used to send a few differentiating DCS between the two panel variants,
 * without exactly knowing what they mean.  These do not seem to be related to
 * panel functionality nor have any visual impact, but they are sent anyway just
 * in case.
 */
enum panel_type {
	/* Sony Xperia 5 */
	PANEL_TYPE_TC01,
	/* Sony Xperia 10 II */
	PANEL_TYPE_UT01,
	/* Sony Xperia 10 III */
	PANEL_TYPE_UT04,
	/* Sony Xperia 10 IV */
	PANEL_TYPE_UT05,
	/* Sony Xperia 10 V and 10 VI */
	PANEL_TYPE_DK01,
};

const struct regulator_bulk_data samsung_sofef01_m_supplies[] = {
	{ .supply = "vddio", /* 1.8 V */ },
	{ .supply = "vci", /* 3.0 V */ },
};

struct samsung_sofef01_m {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	const struct drm_display_mode *mode;
	enum panel_type panel_type;
};

static inline struct samsung_sofef01_m *
to_samsung_sofef01_m(struct drm_panel *panel)
{
	return container_of(panel, struct samsung_sofef01_m, panel);
}

static void samsung_sofef01_m_reset(struct samsung_sofef01_m *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int samsung_sofef01_m_program(struct samsung_sofef01_m *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	dsi_ctx.dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	/* TC01 & UT01 require 10ms, UT04 11ms, and the rest 120ms */
	mipi_dsi_msleep(&dsi_ctx, 120);

	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	if (ctx->panel_type == PANEL_TYPE_TC01 ||
	    ctx->panel_type == PANEL_TYPE_UT01 ||
	    ctx->panel_type == PANEL_TYPE_UT04) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xdf, 0x03);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe0, 0x01);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	}

	if (ctx->panel_type == PANEL_TYPE_UT04) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xE1, 0x00, 0x00, 0x02, 0x00, 0x1C, 0x1C,
					     0x00, 0x00, 0x20, 0x00, 0x00, 0x01, 0x19);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0xa5, 0xa5);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	}

	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0, 1080 - 1);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0, 2520 - 1);

	if (ctx->panel_type == PANEL_TYPE_UT05 || ctx->panel_type == PANEL_TYPE_DK01) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x27, 0xf2);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x80);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x07);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		/* Downstream: ERR_FG Enable */
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe5, 0x15);
		if (ctx->panel_type == PANEL_TYPE_DK01)
			mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xed, 0x0f, 0x4c, 0x20);
		else
			mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xed, 0x04, 0x4c, 0x20);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x02, 0x8f);

		if (ctx->panel_type == PANEL_TYPE_DK01)
			/* Downstream Xperia 10 V: FLM1,FLM2 On */
			mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8f, 0x27, 0x25);
		else if (0) /* TODO: Both use the DK01 panel */
			/* Downstream Xperia 10 VI: FLM1 On, FLM2 On */
			mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8f, 0x27, 0x27);
		else
			/* Downsteam: FLM1 on, FLM2 off */
			mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8f, 0x27, 0x05);

		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x92, 0x63);
		/* Downstream: dimming speed setting */
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x63, 0x05);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	}

	if (ctx->panel_type == PANEL_TYPE_UT05 || ctx->panel_type == PANEL_TYPE_DK01) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					     WRITE_CONTROL_DISPLAY_BACKLIGHT
					     | WRITE_CONTROL_DISPLAY_DIMMING);
	} else {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					     WRITE_CONTROL_DISPLAY_BACKLIGHT);
	}

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x00);

	if (ctx->panel_type != PANEL_TYPE_UT05 && ctx->panel_type != PANEL_TYPE_DK01) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);

		if (ctx->panel_type == PANEL_TYPE_TC01 || ctx->panel_type == PANEL_TYPE_UT01)
			mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x92, 0x29);
		else if (ctx->panel_type == PANEL_TYPE_UT04)
			mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x92, 0x09);

		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x06);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb6, 0x90);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	}

	mipi_dsi_msleep(&dsi_ctx, 110);

	return dsi_ctx.accum_err;
}

static int samsung_sofef01_m_prepare(struct drm_panel *panel)
{
	struct samsung_sofef01_m *ctx = to_samsung_sofef01_m(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(samsung_sofef01_m_supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	samsung_sofef01_m_reset(ctx);

	ret = samsung_sofef01_m_program(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to program panel: %d\n", ret);
		goto fail;
	}

	return 0;

fail:
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(samsung_sofef01_m_supplies), ctx->supplies);
	return ret;
}

static int samsung_sofef01_m_enable(struct drm_panel *panel)
{
	struct samsung_sofef01_m *ctx = to_samsung_sofef01_m(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_usleep_range(&dsi_ctx, 16000, 17000);

	return dsi_ctx.accum_err;
}

static int samsung_sofef01_m_disable(struct drm_panel *panel)
{
	struct samsung_sofef01_m *ctx = to_samsung_sofef01_m(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_usleep_range(&dsi_ctx, 120000, 121000);

	return dsi_ctx.accum_err;
}

static int samsung_sofef01_m_unprepare(struct drm_panel *panel)
{
	struct samsung_sofef01_m *ctx = to_samsung_sofef01_m(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_usleep_range(&dsi_ctx, 100000, 101000);

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(samsung_sofef01_m_supplies), ctx->supplies);

	return dsi_ctx.accum_err;
}

static int samsung_sofef01_m_get_modes(struct drm_panel *panel,
				       struct drm_connector *connector)
{
	struct samsung_sofef01_m *ctx = to_samsung_sofef01_m(panel);

	return drm_connector_helper_get_modes_fixed(connector, ctx->mode);
}

static const struct drm_panel_funcs samsung_sofef01_m_panel_funcs = {
	.prepare = samsung_sofef01_m_prepare,
	.enable = samsung_sofef01_m_enable,
	.disable = samsung_sofef01_m_disable,
	.unprepare = samsung_sofef01_m_unprepare,
	.get_modes = samsung_sofef01_m_get_modes,
};

static int samsung_sofef01_m_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);

	return mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
}

static int samsung_sofef01_m_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);

	if (ret < 0)
		return ret;

	return brightness;
}

static const struct backlight_ops samsung_sofef01_m_bl_ops = {
	.update_status = samsung_sofef01_m_bl_update_status,
	.get_brightness = samsung_sofef01_m_bl_get_brightness,
};

/*
 * drm/msm's DSI code does not calculate transfer time but instead relies on
 * fake porch values (which are not a thing in CMD mode) to represent the
 * transfer time.
 *
 * Use the following expressions based on qcom,mdss-dsi-panel-clockrate from
 * downstream DT to artificially bump the mode's clock that reflects the
 * necessary transfer time / overhead.
 */
static const unsigned int dsi_lanes = 4;
static const unsigned int bpp = 24;
/* qcom,mdss-dsi-panel-clockrate from downstream DT */
static const unsigned long bitclk_hz = 1132293600;
static const unsigned long stable_clockrate = bitclk_hz * dsi_lanes / bpp;
static const unsigned long fake_porch = stable_clockrate / (2520 * 60) - 1080;

/* 61x142mm variant, Sony Xperia 5 */
static const struct drm_display_mode samsung_sofef01_m_61_142_mode = {
	.clock = (1080 + fake_porch) * 2520 * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + fake_porch,
	.hsync_end = 1080 + fake_porch,
	.htotal = 1080 + fake_porch,
	.vdisplay = 2520,
	.vsync_start = 2520,
	.vsync_end = 2520,
	.vtotal = 2520,
	.width_mm = 61,
	.height_mm = 142,
	.type = DRM_MODE_TYPE_DRIVER,
};

/* 61x141mm variant, Sony Xperia 10 V and 10 VI */
static const struct drm_display_mode samsung_sofef01_m_61_141_mode = {
	.clock = (1080 + fake_porch) * 2520 * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + fake_porch,
	.hsync_end = 1080 + fake_porch,
	.htotal = 1080 + fake_porch,
	.vdisplay = 2520,
	.vsync_start = 2520,
	.vsync_end = 2520,
	.vtotal = 2520,
	.width_mm = 61,
	.height_mm = 141,
	.type = DRM_MODE_TYPE_DRIVER,
};

/* 60x139mm variant, Sony Xperia 10 II, 10 III and 10 IV */
static const struct drm_display_mode samsung_sofef01_m_60_139_mode = {
	.clock = (1080 + fake_porch) * 2520 * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + fake_porch,
	.hsync_end = 1080 + fake_porch,
	.htotal = 1080 + fake_porch,
	.vdisplay = 2520,
	.vsync_start = 2520,
	.vsync_end = 2520,
	.vtotal = 2520,
	.width_mm = 60,
	.height_mm = 139,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int samsung_sofef01_m_probe(struct mipi_dsi_device *dsi)
{
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 100,
		.max_brightness = 1023,
	};
	struct device *dev = &dsi->dev;
	struct samsung_sofef01_m *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct samsung_sofef01_m, panel,
				   &samsung_sofef01_m_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(
		dev,
		ARRAY_SIZE(samsung_sofef01_m_supplies),
		samsung_sofef01_m_supplies,
		&ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	ctx->panel_type = (enum panel_type)of_device_get_match_data(dev);
	if (ctx->panel_type == PANEL_TYPE_TC01)
		ctx->mode = &samsung_sofef01_m_61_142_mode;
	else if (ctx->panel_type == PANEL_TYPE_DK01)
		ctx->mode = &samsung_sofef01_m_61_141_mode;
	else
		ctx->mode = &samsung_sofef01_m_60_139_mode;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = devm_backlight_device_register(
		dev, dev_name(dev), dev, dsi,
		&samsung_sofef01_m_bl_ops,
		&props);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void samsung_sofef01_m_remove(struct mipi_dsi_device *dsi)
{
	struct samsung_sofef01_m *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id samsung_sofef01_m_of_match[] = {
	{ .compatible = "samsung,sofef01-m-amb609tc01",
	  .data = (void *)PANEL_TYPE_TC01 },
	{ .compatible = "samsung,sofef01-m-ams597ut01",
	  .data = (void *)PANEL_TYPE_UT01 },
	{ .compatible = "samsung,sofef01-m-ams597ut04",
	  .data = (void *)PANEL_TYPE_UT04 },
	{ .compatible = "samsung,sofef01-m-ams597ut05",
	  .data = (void *)PANEL_TYPE_UT05 },
	{ .compatible = "samsung,sofef01-m-ams605dk01",
	  .data = (void *)PANEL_TYPE_DK01 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, samsung_sofef01_m_of_match);

static struct mipi_dsi_driver samsung_sofef01_m_driver = {
	.probe = samsung_sofef01_m_probe,
	.remove = samsung_sofef01_m_remove,
	.driver = {
		.name = "panel-samsung-sofef01-m",
		.of_match_table = samsung_sofef01_m_of_match,
	},
};
module_mipi_dsi_driver(samsung_sofef01_m_driver);

MODULE_AUTHOR("Marijn Suijten <marijn.suijten@somainline.org>");
MODULE_DESCRIPTION("DRM panel driver for Samsung SOFEF01-M Display-Driver-IC panels");
MODULE_LICENSE("GPL");
