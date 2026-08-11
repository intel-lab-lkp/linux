// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Novatek NT37703 based MIPI DSI panels
 *
 * Multiple panel support based on Novatek NT36523 driver
 * Per panel DSC support based on Ilitek ILI9882T driver
 *
 * Copyright (c) 2026 Esteban Urrutia <esteuwu@proton.me>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#define DSC_BPG_OFFSET(x)		((u8) ((x) & DSC_RANGE_BPG_OFFSET_MASK))
#define NT37703_DCS_SWITCH_PAGE		0xf0

#define nt37703_cmd_test_on(ctx)	mipi_dsi_dcs_write_seq_multi((ctx), 0xff, 0xaa, 0x55, \
								     0xa5, 0x80)
#define nt37703_switch_page(ctx, page)	mipi_dsi_dcs_write_seq_multi((ctx), \
								     NT37703_DCS_SWITCH_PAGE, \
								     0x55, 0xaa, 0x52, 0x08, (page))

struct panel_info {
	struct drm_panel panel;
	const struct panel_desc *desc;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
};

struct panel_desc {
	unsigned int width_mm;
	unsigned int height_mm;

	unsigned int lanes;
	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;

	const struct drm_display_mode *modes;
	unsigned int num_modes;
	int (*init_sequence)(struct panel_info *pinfo);
	const struct drm_dsc_config *dsc;
};

/*
 * These parameters are applied when using DSC 1.1 and when the DSC configuration specifies 10 b.p.c
 * and 8 b.p.p
 */
static const struct drm_dsc_rc_range_parameters nt37703_rc_range_params[DSC_NUM_BUF_RANGES] = {
	{ 0,  8,  DSC_BPG_OFFSET(2)   },
	{ 4,  8,  DSC_BPG_OFFSET(0)   },
	{ 5,  9,  DSC_BPG_OFFSET(0)   },
	{ 5,  10, DSC_BPG_OFFSET(-2)  },
	{ 7,  11, DSC_BPG_OFFSET(-4)  },
	{ 7,  11, DSC_BPG_OFFSET(-6)  },
	{ 7,  11, DSC_BPG_OFFSET(-8)  },
	{ 7,  12, DSC_BPG_OFFSET(-8)  },
	{ 7,  13, DSC_BPG_OFFSET(-8)  },
	{ 8,  14, DSC_BPG_OFFSET(-10) },
	{ 9,  14, DSC_BPG_OFFSET(-10) },
	{ 9,  15, DSC_BPG_OFFSET(-10) },
	{ 9,  15, DSC_BPG_OFFSET(-12) },
	{ 12, 16, DSC_BPG_OFFSET(-12) },
	{ 16, 17, DSC_BPG_OFFSET(-12) },
};

static const struct regulator_bulk_data nt37703_supplies[] = {
	{ .supply = "avdd"  },
	{ .supply = "elvdd" },
	{ .supply = "elvss" },
	{ .supply = "vci"   },
	{ .supply = "vdd"   },
	{ .supply = "vddi"  },
};

static inline struct panel_info *to_panel_info(struct drm_panel *panel)
{
	return container_of_const(panel, struct panel_info, panel);
}

static void nt37703_reset(struct panel_info *pinfo)
{
	gpiod_set_value_cansleep(pinfo->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(pinfo->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(pinfo->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int nt37703_off(struct panel_info *pinfo)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int nt37703_prepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	struct device *dev = &pinfo->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;
	int i;

	ret = regulator_bulk_enable(ARRAY_SIZE(nt37703_supplies), pinfo->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	nt37703_reset(pinfo);

	ret = pinfo->desc->init_sequence(pinfo);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(pinfo->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(nt37703_supplies), pinfo->supplies);
		return ret;
	}

	/*
	 * For some reason this DDIC uses different parameters for the RC ranges under certain DSC
	 * configurations.
	 * Not applying them results in heavy visual glitches, so do so if all conditions are met.
	 */
	if (pinfo->dsc.dsc_version_major == 1 && pinfo->dsc.dsc_version_minor == 1 &&
	    pinfo->dsc.bits_per_component == 10 && pinfo->dsc.bits_per_pixel == 8 << 4) {
		for (i = 0; i < DSC_NUM_BUF_RANGES; i++) {
			pinfo->dsc.rc_range_params[i].range_min_qp =
				nt37703_rc_range_params[i].range_min_qp;
			pinfo->dsc.rc_range_params[i].range_max_qp =
				nt37703_rc_range_params[i].range_max_qp;
			pinfo->dsc.rc_range_params[i].range_bpg_offset =
				nt37703_rc_range_params[i].range_bpg_offset;
		}
	}

	/*
	 * Given drm_dsc_setup_rc_params() inconditionally overrides certain members in pinfo->dsc
	 * that may have been set in pinfo->desc->dsc, override these members once more with what's
	 * found in pinfo->desc->dsc.
	 */
	if (pinfo->desc->dsc->first_line_bpg_offset)
		pinfo->dsc.first_line_bpg_offset = pinfo->desc->dsc->first_line_bpg_offset;

	drm_dsc_pps_payload_pack(&pps, &pinfo->dsc);

	ret = mipi_dsi_picture_parameter_set(pinfo->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "Failed to transmit PPS: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_compression_mode(pinfo->dsi, true);
	if (ret < 0) {
		dev_err(dev, "Failed to enable compression mode: %d\n", ret);
		return ret;
	}

	msleep(28);

	return 0;
}

static int nt37703_unprepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	struct device *dev = &pinfo->dsi->dev;
	int ret;

	ret = nt37703_off(pinfo);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(pinfo->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(nt37703_supplies), pinfo->supplies);

	return 0;
}

static int nt37703_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int i;

	for (i = 0; i < pinfo->desc->num_modes; i++) {
		const struct drm_display_mode *m = &pinfo->desc->modes[i];
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, m);
		if (!mode) {
			dev_err(panel->dev, "Failed to add mode %ux%u@%u\n", m->hdisplay,
				m->vdisplay, drm_mode_vrefresh(m));
			return -ENOMEM;
		}

		mode->type = DRM_MODE_TYPE_DRIVER;
		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = pinfo->desc->width_mm;
	connector->display_info.height_mm = pinfo->desc->height_mm;

	return pinfo->desc->num_modes;
}

static const struct drm_panel_funcs nt37703_panel_funcs = {
	.prepare = nt37703_prepare,
	.unprepare = nt37703_unprepare,
	.get_modes = nt37703_get_modes,
};

static int nt37703_bl_update_status(struct backlight_device *bl)
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

static int nt37703_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops nt37703_bl_ops = {
	.update_status = nt37703_bl_update_status,
	.get_brightness = nt37703_bl_get_brightness,
};

static struct backlight_device *nt37703_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 2047,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi, &nt37703_bl_ops,
					      &props);
}

static const struct drm_display_mode bronco_tianma_modes[] = {
	{
		.clock = (1080 + 16 + 4 + 16) * (2400 + 44 + 2 + 14) * 144 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 16,
		.hsync_end = 1080 + 16 + 4,
		.htotal = 1080 + 16 + 4 + 16,
		.vdisplay = 2400,
		.vsync_start = 2400 + 44,
		.vsync_end = 2400 + 44 + 2,
		.vtotal = 2400 + 44 + 2 + 14,
	},
};

static int bronco_tianma_v2_init_sequence(struct panel_info *pinfo)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->dsi };

	nt37703_switch_page(&dsi_ctx, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x29);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x2b, 0x1b, 0x00, 0x32);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x88, 0x01, 0x02, 0x1b, 0x08, 0x77);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x87, 0x20);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0, 1080 - 1);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0, 2400 - 1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x03, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	nt37703_cmd_test_on(&dsi_ctx);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x15);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x01, 0xe1);
	nt37703_cmd_test_on(&dsi_ctx);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x17);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x13);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf9, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x3c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf5, 0x81);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_set_display_brightness_multi(&dsi_ctx, 0x0000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2f, 0x00);
	nt37703_switch_page(&dsi_ctx, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc7, 0x47);
	nt37703_switch_page(&dsi_ctx, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0xc4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb2, 0x09, 0x84, 0x09, 0x84, 0x09, 0x84, 0x16,
				     0xd6);
	nt37703_switch_page(&dsi_ctx, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc6, 0x00, 0x18, 0x00, 0x18, 0x00, 0x18, 0x00,
				     0x18);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2f, 0x83);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8b, 0x00, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static const struct drm_dsc_config bronco_tianma_dsc = {
	.dsc_version_major = 1,
	.dsc_version_minor = 1,
	.slice_height = 20,
	.slice_width = 540,
	.slice_count = 2,		/* 1080 / slice_width */
	.bits_per_component = 10,
	.bits_per_pixel = 8 << 4,	/* 4 fractional bits  */
	.block_pred_enable = true,
	.first_line_bpg_offset = 13,
};

static const struct panel_desc bronco_tianma_v2_desc = {
	.modes = bronco_tianma_modes,
	.num_modes = ARRAY_SIZE(bronco_tianma_modes),
	.width_mm = 68,
	.height_mm = 152,
	.lanes = 4,
	.format = MIPI_DSI_FMT_RGB101010,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_NO_EOT_PACKET |
		      MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM |
		      MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT;
	.init_sequence = bronco_tianma_v2_init_sequence,
	.dsc = &bronco_tianma_dsc,
};

static int nt37703_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct panel_info *pinfo;
	int ret;

	pinfo = devm_drm_panel_alloc(dev, struct panel_info, panel, &nt37703_panel_funcs,
				     DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(pinfo))
		return PTR_ERR(pinfo);

	pinfo->desc = of_device_get_match_data(dev);
	if (IS_ERR(pinfo->desc))
		return PTR_ERR(pinfo->desc);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(nt37703_supplies), nt37703_supplies,
					    &pinfo->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pinfo->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pinfo->reset_gpio),
				     "Failed to get reset-gpios\n");

	pinfo->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, pinfo);

	dsi->lanes = pinfo->desc->lanes;
	dsi->format = pinfo->desc->format;
	dsi->mode_flags = pinfo->desc->mode_flags;

	pinfo->panel.prepare_prev_first = true;

	pinfo->panel.backlight = nt37703_create_backlight(dsi);
	if (IS_ERR(pinfo->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(pinfo->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&pinfo->panel);

	/* This panel only supports DSC; unconditionally enable it */
	pinfo->dsc = *(pinfo->desc->dsc);
	dsi->dsc = &pinfo->dsc;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&pinfo->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void nt37703_remove(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&pinfo->panel);
}

static const struct of_device_id nt37703_of_match[] = {
	{
		.compatible = "motorola,bronco-tianma-v2-nt37703",
		.data = &bronco_tianma_v2_desc,
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, nt37703_of_match);

static struct mipi_dsi_driver nt37703_driver = {
	.probe = nt37703_probe,
	.remove = nt37703_remove,
	.driver = {
		.name = "panel-novatek-nt37703",
		.of_match_table = nt37703_of_match,
	},
};
module_mipi_dsi_driver(nt37703_driver);

MODULE_AUTHOR("Esteban Urrutia <esteuwu@proton.me>");
MODULE_DESCRIPTION("DRM driver for Novatek NT37703 based MIPI DSI panels");
MODULE_LICENSE("GPL");
