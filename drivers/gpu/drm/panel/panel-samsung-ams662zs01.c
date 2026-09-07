// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung AMS662ZS01 6.62" 1080x2400 AMOLED DSI command-mode panel with DSC.
 *
 * Found on the OnePlus 9RT (oneplus,martini). Both panel revisions the vendor
 * tree carries -- "samsung ams662zs01 fhd cmd mode dsc dsi panel" and its
 * dvt respin, which the martini bootloader hands off as
 * msm_drm.dsi_display0=qcom,mdss_dsi_samsung_ams662zs01_dvt_dsc_cmd -- share
 * one initialisation sequence and one set of timings; the revisions differ only
 * in the HBM/fingerprint brightness tables, which this driver does not use.
 *
 * Command sequences and timings transcribed from
 * arch/arm64/boot/dts/vendor/qcom/display/dsi-panel-samsung_ams662zs01_dvt_dsc_cmd.dtsi
 * in the vendor kernel (timing0, the 60 Hz mode).
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include <video/mipi_display.h>

/*
 * Samsung DDIC vendor commands. The PPS is written as a DCS long write of the
 * 128-byte payload rather than as a MIPI PPS (0x0a) packet, and compression is
 * armed by a DCS register write rather than by a MIPI compression-mode (0x07)
 * packet -- that is what the vendor sequence does on this DDIC, so it is what
 * is known to work here.
 */
#define AMS662ZS01_DCS_WRITE_PPS		0x9e
#define AMS662ZS01_DCS_COMPRESSION_MODE		0x9d
#define AMS662ZS01_DCS_ACCESS_KEY		0xf0
#define AMS662ZS01_DCS_FREQ_SELECT		0x60
#define AMS662ZS01_DCS_GAMMA_UPDATE		0xf7

/* Payloads of AMS662ZS01_DCS_FREQ_SELECT, from the per-timing switch commands. */
#define AMS662ZS01_FREQ_60HZ			0x00
#define AMS662ZS01_FREQ_120HZ			0x08

struct ams662zs01 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *vout_gpio;
	struct regulator_bulk_data *supplies;
};

/*
 * vddio is pm8350c L12C at 1.8 V, vdd is pm8350c L13C at 3.0-3.2 V. The vendor
 * tree wires both through the DSI controller node (display/lahaina-sde-display.dtsi)
 * and lists them as the panel's supply entries; lab/ibb are commented out on
 * oplus builds, so the pm8350b AMOLED AB/IBB regulators are not involved and
 * need no driver here.
 */
static const struct regulator_bulk_data ams662zs01_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vdd" },
};

static inline struct ams662zs01 *to_ams662zs01(struct drm_panel *panel)
{
	return container_of(panel, struct ams662zs01, panel);
}

/*
 * qcom,mdss-dsi-reset-sequence = <1 10>, <0 10>, <1 10>: the pin idles high,
 * is pulled low for 10 ms and released. reset-gpios is active-low, so the
 * logical values run the other way round.
 */
static void ams662zs01_reset(struct ams662zs01 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

/* The 0xf0 key gates every vendor register touched below. */
#define ams662zs01_unlock(dsi_ctx) \
	mipi_dsi_dcs_write_seq_multi((dsi_ctx), AMS662ZS01_DCS_ACCESS_KEY, 0x5a, 0x5a)
#define ams662zs01_lock(dsi_ctx) \
	mipi_dsi_dcs_write_seq_multi((dsi_ctx), AMS662ZS01_DCS_ACCESS_KEY, 0xa5, 0xa5)

/*
 * The payload is built at run time rather than with
 * mipi_dsi_dcs_write_seq_multi(): that macro stores its arguments in a
 * "static const u8" array, so every one of them has to be a compile-time
 * constant and a variable freq does not compile.
 */
static void ams662zs01_set_freq(struct mipi_dsi_multi_context *dsi_ctx, u8 freq)
{
	const u8 freq_select[] = { AMS662ZS01_DCS_FREQ_SELECT, freq, 0x00 };

	ams662zs01_unlock(dsi_ctx);
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, freq_select, sizeof(freq_select));
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, AMS662ZS01_DCS_GAMMA_UPDATE, 0x0f);
	ams662zs01_lock(dsi_ctx);
}

static void ams662zs01_send_pps(struct mipi_dsi_multi_context *dsi_ctx,
				const struct drm_dsc_config *dsc)
{
	struct drm_dsc_picture_parameter_set pps;
	u8 buf[1 + sizeof(pps)];

	drm_dsc_pps_payload_pack(&pps, dsc);

	buf[0] = AMS662ZS01_DCS_WRITE_PPS;
	memcpy(&buf[1], &pps, sizeof(pps));

	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, buf, sizeof(buf));
}

static int ams662zs01_prepare(struct drm_panel *panel)
{
	struct ams662zs01 *ctx = to_ams662zs01(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	dsi_ctx.accum_err = regulator_bulk_enable(ARRAY_SIZE(ams662zs01_supplies),
						  ctx->supplies);
	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	/*
	 * Panel VOUT enable (tlmm 25). The vendor driver raises it right after
	 * the rails and the pinctrl state, and drops it in power-off
	 * (techpack/display/msm/dsi/dsi_panel.c, panel_vout_gpio).
	 */
	gpiod_set_value_cansleep(ctx->vout_gpio, 1);
	usleep_range(1000, 2000);

	ams662zs01_reset(ctx);

	/*
	 * ctx->dsc is filled in by the DSI host: dsi_timing_setup() runs
	 * dsi_populate_dsc_params() on the panel's config before the panel is
	 * prepared (panel.prepare_prev_first), so the rate-control parameters
	 * the PPS carries are computed by then.
	 */
	ams662zs01_send_pps(&dsi_ctx, ctx->dsi->dsc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc2, 0x14);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, AMS662ZS01_DCS_COMPRESSION_MODE, 0x01);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 128);

	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0, 1080 - 1);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0, 2400 - 1);

	/* FQ CON: pick the fixed-frequency (non-adaptive) frame rate path. */
	ams662zs01_unlock(&dsi_ctx);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x27, 0xf2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x00);
	ams662zs01_lock(&dsi_ctx);

	ams662zs01_set_freq(&dsi_ctx, AMS662ZS01_FREQ_60HZ);

	/* Brightness control on, dimming off. */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		gpiod_set_value_cansleep(ctx->vout_gpio, 0);
		regulator_bulk_disable(ARRAY_SIZE(ams662zs01_supplies),
				       ctx->supplies);
	}

	return dsi_ctx.accum_err;
}

static int ams662zs01_enable(struct drm_panel *panel)
{
	struct ams662zs01 *ctx = to_ams662zs01(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int ams662zs01_disable(struct drm_panel *panel)
{
	struct ams662zs01 *ctx = to_ams662zs01(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* Leave AOD, in case the panel was put there behind our back. */
	ams662zs01_unlock(&dsi_ctx);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x91, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	ams662zs01_lock(&dsi_ctx);

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 11);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 121);

	return dsi_ctx.accum_err;
}

static int ams662zs01_unprepare(struct drm_panel *panel)
{
	struct ams662zs01 *ctx = to_ams662zs01(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	gpiod_set_value_cansleep(ctx->vout_gpio, 0);

	regulator_bulk_disable(ARRAY_SIZE(ams662zs01_supplies), ctx->supplies);

	return 0;
}

/*
 * timing0 of the vendor panel node: 60 Hz, hfp 64 / hpw 8 / hbp 48,
 * vfp 8 / vpw 4 / vbp 12.
 *
 * The 120 Hz timing1 (hfp 8 / hpw 24 / hbp 8, vfp 2 / vpw 2 / vbp 8) is
 * deliberately not advertised. Switching rate on this DDIC needs its
 * AMS662ZS01_DCS_FREQ_SELECT payload changed to AMS662ZS01_FREQ_120HZ in the
 * same breath as the CRTC timing, and drm_panel_funcs has no mode_set hook to
 * hang that off, so a second mode would give DPU 120 Hz timings while the panel
 * stayed at 60.
 */
static const struct drm_display_mode ams662zs01_mode = {
	.clock = (1080 + 64 + 8 + 48) * (2400 + 8 + 4 + 12) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 64,
	.hsync_end = 1080 + 64 + 8,
	.htotal = 1080 + 64 + 8 + 48,
	.vdisplay = 2400,
	.vsync_start = 2400 + 8,
	.vsync_end = 2400 + 8 + 4,
	.vtotal = 2400 + 8 + 4 + 12,
	.width_mm = 70,
	.height_mm = 153,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int ams662zs01_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &ams662zs01_mode);
}

static const struct drm_panel_funcs ams662zs01_panel_funcs = {
	.prepare = ams662zs01_prepare,
	.enable = ams662zs01_enable,
	.disable = ams662zs01_disable,
	.unprepare = ams662zs01_unprepare,
	.get_modes = ams662zs01_get_modes,
};

static int ams662zs01_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret;
}

static const struct backlight_ops ams662zs01_bl_ops = {
	.update_status = ams662zs01_bl_update_status,
};

/*
 * qcom,mdss-dsi-bl-max-level = <4095>, default level 1600. The DDIC takes the
 * 12-bit value as a big-endian 16-bit DCS 0x51 payload, which is what
 * mipi_dsi_dcs_set_display_brightness_large() emits.
 */
static struct backlight_device *ams662zs01_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1600,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &ams662zs01_bl_ops, &props);
}

static int ams662zs01_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ams662zs01 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ams662zs01, panel,
				   &ams662zs01_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(ams662zs01_supplies),
					    ams662zs01_supplies, &ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->vout_gpio = devm_gpiod_get(dev, "vout", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->vout_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->vout_gpio),
				     "Failed to get vout-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS |
			  MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = ams662zs01_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/*
	 * DSC 1.1, two 540x30 slices per line, 8 bpc, 8 bpp -- 3:1 against the
	 * 24 bpp source. The panel has no uncompressed mode.
	 */
	dsi->dsc = &ctx->dsc;
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_height = 30;
	ctx->dsc.slice_width = 540;
	ctx->dsc.slice_count = 1080 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void ams662zs01_remove(struct mipi_dsi_device *dsi)
{
	struct ams662zs01 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ams662zs01_of_match[] = {
	{ .compatible = "samsung,ams662zs01" },
	{ }
};
MODULE_DEVICE_TABLE(of, ams662zs01_of_match);

static struct mipi_dsi_driver ams662zs01_driver = {
	.probe = ams662zs01_probe,
	.remove = ams662zs01_remove,
	.driver = {
		.name = "panel-samsung-ams662zs01",
		.of_match_table = ams662zs01_of_match,
	},
};
module_mipi_dsi_driver(ams662zs01_driver);

MODULE_DESCRIPTION("DRM driver for the Samsung AMS662ZS01 DSC command-mode panel");
MODULE_LICENSE("GPL");
