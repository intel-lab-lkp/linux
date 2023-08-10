// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Ilitek ILI9806E a-Si TFT LCD controller.
 *
 * Copyright (c) 2023 Delcon SRL
 * Luca Ceresoli <luca.ceresoli@bootlin.com>
 */

#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>

#define ILI9806E_BUS_FORMAT	MEDIA_BUS_FMT_RGB666_1X18

// Page 1 registers
#define ILI9806E_P1_IFMODE1	0x08	// Interface Mode Control 1
#define             IFMODE1_SEPT_SDIO	BIT(3) // 1 = two data pins
#define             IFMODE1_SDO_STATUS	BIT(4) // 0 = SDO has output enable
#define ILI9806E_P1_DISCTRL1	0x20	// Display Function Control 1
#define             DISCTRL1_SYNC_MODE	BIT(0) // RGB interface mode: 0 = DE mode, 1 = SYNC mode
#define ILI9806E_P1_DISCTRL2	0x21	// Display Function Control 2
#define             DISCTRL2_EPL	BIT(0) // DE polarity (1 = active high)
#define             DISCTRL2_DPL	BIT(1) // PCLK polarity (1 = fetch on falling edge)
#define             DISCTRL2_HSPL	BIT(2) // HS polarity (1 = active high)
#define             DISCTRL2_VSPL	BIT(3) // VS polarity (1 = active high)
#define ILI9806E_P1_RESCTRL	0x30	// Resolution Control
#define             RESCTRL_480x864	0x0
#define             RESCTRL_480x854	0x1
#define             RESCTRL_480x800	0x2
#define             RESCTRL_480x640	0x3
#define             RESCTRL_480x720	0x4
#define ILI9806E_P1_INVTR	0x31	// Display Inversion Control
#define             INVTR_NLA_COLUMN	0x0
#define             INVTR_NLA_1DOT	0x1
#define             INVTR_NLA_2DOT	0x2
#define             INVTR_NLA_3DOT	0x3
#define             INVTR_NLA_4DOT	0x4
#define ILI9806E_P1_PWCTRL1	0x40	// Power Control 1
#define ILI9806E_P1_PWCTRL2	0x41	// Power Control 2
#define ILI9806E_P1_PWCTRL3	0x42	// Power Control 3
#define ILI9806E_P1_PWCTRL4	0x43	// Power Control 4
#define ILI9806E_P1_PWCTRL5	0x44	// Power Control 5
#define ILI9806E_P1_PWCTRL6	0x45	// Power Control 6
#define ILI9806E_P1_PWCTRL7	0x46	// Power Control 7
#define ILI9806E_P1_PWCTRL8	0x47	// Power Control 8
#define ILI9806E_P1_PWCTRL9	0x50	// Power Control 9
#define ILI9806E_P1_PWCTRL10	0x51	// Power Control 10
#define ILI9806E_P1_VMCTRL1	0x52	// VCOM Control 1
#define ILI9806E_P1_VMCTRL2	0x53	// VCOM Control 1
#define ILI9806E_P1_SRCTADJ1	0x60	// Source Timing Adjust 1
#define ILI9806E_P1_SRCTADJ2	0x61	// Source Timing Adjust 2
#define ILI9806E_P1_SRCTADJ3	0x62	// Source Timing Adjust 3
#define ILI9806E_P1_SRCTADJ4	0x63	// Source Timing Adjust 4
#define ILI9806E_P1_P_GAMMA(n)	(0xa0 + (n) - 1) // Positive Gamma Control 1~16
#define ILI9806E_P1_N_GAMMA(n)	(0xc0 + (n) - 1) // Negative Gamma Correction 1~16

// Page 7 registers
#define ILI9806E_P7_VGLREGEN	0x17	// VGL_REG EN
#define ILI9806E_P7_0x02	0x02	// undocumented
#define ILI9806E_P7_0xe1	0xe1	// undocumented

// The page-switching register (valid for all pages)
#define ILI9806E_Px_ENEXTC	0xff

static const struct drm_display_mode nds040480800_v3_mode = {
	.width_mm    = 51,
	.height_mm   = 85,
	.clock       = 30000,
	.hdisplay    = 480,
	.hsync_start = 480 + 25,
	.hsync_end   = 480 + 25 + 54,
	.htotal      = 480 + 25 + 54 + 25,
	.vdisplay    = 800,
	.vsync_start = 800 + 25,
	.vsync_end   = 800 + 25 + 14,
	.vtotal      = 800 + 25 + 14 + 22,
	.flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.type        = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER,
};

struct ili9806e {
	struct mipi_dbi dbi;
	struct drm_panel panel;
};

static inline struct ili9806e *panel_to_ili9806e(struct drm_panel *panel)
{
	return container_of(panel, struct ili9806e, panel);
}

static int ili9806e_switch_page(struct ili9806e *ctx, unsigned int page)
{
	return mipi_dbi_command(&ctx->dbi, ILI9806E_Px_ENEXTC, 0xff, 0x98, 0x06, 0x04, page);
}

static int ili9806e_unprepare(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	struct mipi_dbi *dbi = &ctx->dbi;

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF, 0x00);
	mipi_dbi_command(dbi, MIPI_DCS_ENTER_SLEEP_MODE, 0x00);

	return 0;
}

static int ili9806e_prepare(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	struct mipi_dbi *dbi = &ctx->dbi;

	/* Reset */

	gpiod_set_value(ctx->dbi.reset, 1);
	usleep_range(15, 50); // Min 10 us
	gpiod_set_value(ctx->dbi.reset, 0);
	msleep(125); // Min 5 ms in sleep in mode, 120 ms in sleep out mode

	/* Init sequence */

	ili9806e_switch_page(ctx, 1);

	mipi_dbi_command(dbi, ILI9806E_P1_IFMODE1,  IFMODE1_SDO_STATUS);
	mipi_dbi_command(dbi, ILI9806E_P1_DISCTRL2, DISCTRL2_EPL);
	mipi_dbi_command(dbi, ILI9806E_P1_RESCTRL,  RESCTRL_480x800);
	mipi_dbi_command(dbi, ILI9806E_P1_INVTR,    INVTR_NLA_COLUMN);

	mipi_dbi_command(dbi, ILI9806E_P1_PWCTRL1,  0x10);
	mipi_dbi_command(dbi, ILI9806E_P1_PWCTRL2,  0x55);
	mipi_dbi_command(dbi, ILI9806E_P1_PWCTRL3,  0x02);
	mipi_dbi_command(dbi, ILI9806E_P1_PWCTRL4,  0x09);
	mipi_dbi_command(dbi, ILI9806E_P1_PWCTRL5,  0x07);
	mipi_dbi_command(dbi, ILI9806E_P1_PWCTRL9,  0x78);
	mipi_dbi_command(dbi, ILI9806E_P1_PWCTRL10, 0x78);

	mipi_dbi_command(dbi, ILI9806E_P1_VMCTRL1,  0x00);
	mipi_dbi_command(dbi, ILI9806E_P1_VMCTRL2,  0x6d);

	mipi_dbi_command(dbi, ILI9806E_P1_SRCTADJ1, 0x07);
	mipi_dbi_command(dbi, ILI9806E_P1_SRCTADJ2, 0x00);
	mipi_dbi_command(dbi, ILI9806E_P1_SRCTADJ3, 0x08);
	mipi_dbi_command(dbi, ILI9806E_P1_SRCTADJ4, 0x00);

	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(1),  0x00);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(2),  0x07);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(3),  0x0c);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(4),  0x0b);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(5),  0x03);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(6),  0x07);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(7),  0x06);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(8),  0x04);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(9),  0x08);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(10), 0x0c);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(11), 0x13);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(12), 0x06);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(13), 0x0d);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(14), 0x19);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(15), 0x10);
	mipi_dbi_command(dbi, ILI9806E_P1_P_GAMMA(16), 0x00);

	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(1),  0x00);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(2),  0x07);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(3),  0x0c);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(4),  0x0b);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(5),  0x03);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(6),  0x07);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(7),  0x07);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(8),  0x04);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(9),  0x08);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(10), 0x0c);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(11), 0x13);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(12), 0x06);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(13), 0x0d);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(14), 0x18);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(15), 0x10);
	mipi_dbi_command(dbi, ILI9806E_P1_N_GAMMA(16), 0x00);

	ili9806e_switch_page(ctx, 6);

	/* Registers in page 6 are not really documented except for the comments copied below */
	mipi_dbi_command(dbi, 0x00, 0x20); // STV_A_Rise[10:8] | GIP_0_SET0
	mipi_dbi_command(dbi, 0x01, 0x0a); // STV_A_Rise[7:0]
	mipi_dbi_command(dbi, 0x02, 0x00); // GIP_0_SET1
	mipi_dbi_command(dbi, 0x03, 0x00); // GIP_0_SET2
	mipi_dbi_command(dbi, 0x04, 0x01); // GIP_0_SET3
	mipi_dbi_command(dbi, 0x05, 0x01); // GIP_0_SET4
	mipi_dbi_command(dbi, 0x06, 0x98); // CLK_A_Rise[10:8] | GIP_0_SET5
	mipi_dbi_command(dbi, 0x07, 0x06); // CLK_A_Rise[7:0]
	mipi_dbi_command(dbi, 0x08, 0x01); // GIP_0_SET6
	mipi_dbi_command(dbi, 0x09, 0x80); // GIP_0_SET7
	mipi_dbi_command(dbi, 0x0a, 0x00); // GIP_0_SET8
	mipi_dbi_command(dbi, 0x0b, 0x00); // GIP_0_SET9
	mipi_dbi_command(dbi, 0x0c, 0x01); // GIP_0_SET10
	mipi_dbi_command(dbi, 0x0d, 0x01); // GIP_0_SET11
	mipi_dbi_command(dbi, 0x0e, 0x00); // GIP_0_SET12
	mipi_dbi_command(dbi, 0x0f, 0x00); // GIP_0_SET13
	mipi_dbi_command(dbi, 0x10, 0xf0); // GIP_0_SET14
	mipi_dbi_command(dbi, 0x11, 0xf4); // GIP_0_SET15
	mipi_dbi_command(dbi, 0x12, 0x01); // GIP_0_SET16
	mipi_dbi_command(dbi, 0x13, 0x00); // GIP_0_SET17
	mipi_dbi_command(dbi, 0x14, 0x00); // GIP_0_SET18
	mipi_dbi_command(dbi, 0x15, 0xc0); // GIP_0_SET19
	mipi_dbi_command(dbi, 0x16, 0x08); // GIP_0_SET20
	mipi_dbi_command(dbi, 0x17, 0x00); // GIP_0_SET21
	mipi_dbi_command(dbi, 0x18, 0x00); // GIP_0_SET22
	mipi_dbi_command(dbi, 0x19, 0x00); // GIP_0_SET23
	mipi_dbi_command(dbi, 0x1a, 0x00); // GIP_0_SET24
	mipi_dbi_command(dbi, 0x1b, 0x00); // GIP_0_SET25
	mipi_dbi_command(dbi, 0x1c, 0x00); // GIP_0_SET26
	mipi_dbi_command(dbi, 0x1d, 0x00); // GIP_0_SET27
	mipi_dbi_command(dbi, 0x20, 0x01); // GIP_1_SET0
	mipi_dbi_command(dbi, 0x21, 0x23); // GIP_1_SET1
	mipi_dbi_command(dbi, 0x22, 0x45); // GIP_1_SET2
	mipi_dbi_command(dbi, 0x23, 0x67); // GIP_1_SET3
	mipi_dbi_command(dbi, 0x24, 0x01); // GIP_1_SET4
	mipi_dbi_command(dbi, 0x25, 0x23); // GIP_1_SET5
	mipi_dbi_command(dbi, 0x26, 0x45); // GIP_1_SET6
	mipi_dbi_command(dbi, 0x27, 0x67); // GIP_1_SET7
	mipi_dbi_command(dbi, 0x30, 0x11); // GIP_2_SET8
	mipi_dbi_command(dbi, 0x31, 0x11); // GIP_2_SET9
	mipi_dbi_command(dbi, 0x32, 0x00); // GIP_2_SET10
	mipi_dbi_command(dbi, 0x33, 0xee); // GIP_2_SET11
	mipi_dbi_command(dbi, 0x34, 0xff); // GIP_2_SET12
	mipi_dbi_command(dbi, 0x35, 0xbb); // GIP_2_SET13
	mipi_dbi_command(dbi, 0x36, 0xaa); // GIP_2_SET14
	mipi_dbi_command(dbi, 0x37, 0xdd); // GIP_2_SET15
	mipi_dbi_command(dbi, 0x38, 0xcc); // GIP_2_SET16
	mipi_dbi_command(dbi, 0x39, 0x66); // GIP_2_SET17
	mipi_dbi_command(dbi, 0x3a, 0x77); // GIP_2_SET18
	mipi_dbi_command(dbi, 0x3b, 0x22); // GIP_2_SET19
	mipi_dbi_command(dbi, 0x3c, 0x22); // GIP_2_SET20
	mipi_dbi_command(dbi, 0x3d, 0x22); // GIP_2_SET21
	mipi_dbi_command(dbi, 0x3e, 0x22); // GIP_2_SET22
	mipi_dbi_command(dbi, 0x3f, 0x22); // GIP_2_SET23
	mipi_dbi_command(dbi, 0x40, 0x22); // GIP_2_SET24
	mipi_dbi_command(dbi, 0x52, 0x10); // undocumented
	mipi_dbi_command(dbi, 0x53, 0x10); // GOUT_VGLO Control

	ili9806e_switch_page(ctx, 7);

	mipi_dbi_command(dbi, ILI9806E_P7_VGLREGEN, 0x22);
	mipi_dbi_command(dbi, ILI9806E_P7_0x02,     0x77);
	mipi_dbi_command(dbi, ILI9806E_P7_0xe1,     0x79);

	ili9806e_switch_page(ctx, 0);

	mipi_dbi_command(dbi, MIPI_DCS_SET_TEAR_ON);
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);

	msleep(120);

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);

	return 0;
}

static int ili9806e_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	const u32 bus_format = ILI9806E_BUS_FORMAT;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &nds040480800_v3_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_display_info_set_bus_formats(&connector->display_info, &bus_format, 1);

	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs ili9806e_drm_funcs = {
	.unprepare = ili9806e_unprepare,
	.prepare   = ili9806e_prepare,
	.get_modes = ili9806e_get_modes,
};

static int ili9806e_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct ili9806e *ctx;
	int err;

	ctx = devm_kzalloc(dev, sizeof(struct ili9806e), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	drm_panel_init(&ctx->panel, dev, &ili9806e_drm_funcs, DRM_MODE_CONNECTOR_DPI);

	spi_set_drvdata(spi, ctx);

	ctx->dbi.reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->dbi.reset))
		return dev_err_probe(dev, PTR_ERR(ctx->dbi.reset), "cannot get reset-gpios\n");

	err = drm_panel_of_backlight(&ctx->panel);
	if (err)
		return dev_err_probe(dev, err, "Failed to get backlight\n");

	err = mipi_dbi_spi_init(spi, &ctx->dbi, NULL);
	if (err)
		return dev_err_probe(dev, err, "MIPI DBI init failed\n");

	drm_panel_add(&ctx->panel);

	return 0;
}

static void ili9806e_remove(struct spi_device *spi)
{
	struct ili9806e *ctx = spi_get_drvdata(spi);

	drm_panel_remove(&ctx->panel);
}

#ifdef CONFIG_PM_SLEEP
static int ili9806e_suspend(struct device *dev)
{
	struct ili9806e *ctx = dev_get_drvdata(dev);
	struct mipi_dbi *dbi = &ctx->dbi;

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF, 0x00);
	mipi_dbi_command(dbi, MIPI_DCS_ENTER_SLEEP_MODE, 0x00);

	return 0;
}

static int ili9806e_resume(struct device *dev)
{
	struct ili9806e *ctx = dev_get_drvdata(dev);
	struct mipi_dbi *dbi = &ctx->dbi;

	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE, 0x00);
	msleep(120);
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON, 0x00);

	return 0;
}
#endif

static const struct dev_pm_ops ili9806e_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ili9806e_suspend, ili9806e_resume)
};

static const struct of_device_id ili9806e_of_match[] = {
	{ .compatible = "newdisplay,nds040480800-v3" },
	{ }
};
MODULE_DEVICE_TABLE(of, ili9806e_of_match);

static const struct spi_device_id ili9806e_ids[] = {
	{ "nds040480800-v3", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, ili9806e_ids);

static struct spi_driver ili9806e_driver = {
	.probe = ili9806e_probe,
	.remove = ili9806e_remove,
	.id_table = ili9806e_ids,
	.driver = {
		.name = "panel-ilitek-ili9806e",
		.of_match_table = ili9806e_of_match,
		.pm = &ili9806e_pm_ops,
	},
};
module_spi_driver(ili9806e_driver);

MODULE_AUTHOR("Luca Ceresoli <luca.ceresoli@bootlin.com>");
MODULE_DESCRIPTION("Ilitek ILI9806E LCD Driver");
MODULE_LICENSE("GPL");
