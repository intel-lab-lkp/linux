// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT51021 1200x1920 DSI video-mode panel
 *
 * Derived from the vendor LCM driver in the Amazon 3.18 tree,
 * drivers/misc/mediatek/lcm/nt51021_wuxga_dsi_vdo/.
 *
 * Note on the command interface: this panel can take its control commands
 * either over I2C or in-band over MIPI, and on the shipping ("dvt") boards it
 * does NOT default to MIPI. Writing 0xa5 to register 0x8f forces commands to
 * be accepted over DSI, and writing 0x00 hands the interface back. Every
 * command this driver sends therefore has to sit inside such a window --
 * including sleep in/out and display on/off, which is why they are not simply
 * issued from enable()/disable() the way most panel drivers do it.
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

/* Register 0x8f selects where the panel listens for commands. */
#define NT51021_CMD_IF			0x8f
#define NT51021_CMD_IF_FORCE_MIPI	0xa5
#define NT51021_CMD_IF_RELEASE		0x00

struct nt51021_cmd {
	u8 reg;
	u8 val;
	u16 delay_ms;
};

/*
 * Taken verbatim from the vendor's init_lcm_registers() BOE branch, minus the
 * command-window writes, which this driver manages itself.
 */
static const struct nt51021_cmd nt51021_init_suez_boe[] = {
	{ 0x01, 0x00, 20 },	/* software reset */
	{ 0x83, 0x00, 0 },	/* page select */
	{ 0x84, 0x00, 0 },
	{ 0x8c, 0x80, 0 },	/* GOP setting */
	{ 0xcd, 0x6c, 0 },	/* 3 dummy */
	{ 0xc0, 0x8b, 0 },	/* GCH */
	{ 0xc8, 0xf0, 0 },	/* GCH */
	{ 0x97, 0x00, 0 },	/* resistor setting, 100 ohm */
	{ 0x8b, 0x10, 0 },
	{ 0xa9, 0x20, 0 },	/* enable TP_SYNC */
	{ 0x83, 0xaa, 0 },	/* page select */
	{ 0x84, 0x11, 0 },
	{ 0xa9, 0x4b, 0 },	/* MIPI Rx drive strength, 85% */
	{ 0x85, 0x04, 0 },	/* test mode 1 */
	{ 0x86, 0x08, 0 },	/* test mode 2 */
	{ 0x9c, 0x10, 0 },	/* test mode 3 */
};

struct nt51021_desc {
	const struct nt51021_cmd *cmds;
	unsigned int num_cmds;
};

static const struct nt51021_desc suez_boe_desc = {
	.cmds = nt51021_init_suez_boe,
	.num_cmds = ARRAY_SIZE(nt51021_init_suez_boe),
};

struct nt51021 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *supply;
	struct gpio_desc *reset_gpio;
	const struct nt51021_desc *desc;
};

static inline struct nt51021 *to_nt51021(struct drm_panel *panel)
{
	return container_of(panel, struct nt51021, panel);
}

/*
 * A plain "write this value to that register" DCS write. The
 * mipi_dsi_dcs_write_seq_multi() macro cannot be used for these: it stashes its
 * payload in a static const array, so every byte has to be a compile-time
 * constant, and both the register and the value here come from a table.
 */
static void nt51021_write(struct mipi_dsi_multi_context *ctx, u8 reg, u8 val)
{
	const u8 d[] = { reg, val };

	mipi_dsi_dcs_write_buffer_multi(ctx, d, sizeof(d));
}

/*
 * Open or close the in-band command window. Outside it the panel ignores DSI
 * commands entirely, so every caller that wants to be heard must wrap its
 * writes in a pair of these.
 */
static void nt51021_cmd_window(struct mipi_dsi_multi_context *ctx, bool open)
{
	nt51021_write(ctx, NT51021_CMD_IF,
		      open ? NT51021_CMD_IF_FORCE_MIPI : NT51021_CMD_IF_RELEASE);
	if (open)
		mipi_dsi_msleep(ctx, 1);
}

static int nt51021_prepare(struct drm_panel *panel)
{
	struct nt51021 *ctx = to_nt51021(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	unsigned int i;
	int ret;

	ret = regulator_enable(ctx->supply);
	if (ret)
		return ret;

	/*
	 * Vendor timing: reset held low for 20 ms, then 5 ms of settling before
	 * the first command. The GPIO is active low in the device tree, so
	 * "1" here means asserted.
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);

	nt51021_cmd_window(&dsi_ctx, true);

	for (i = 0; i < ctx->desc->num_cmds; i++) {
		const struct nt51021_cmd *cmd = &ctx->desc->cmds[i];

		nt51021_write(&dsi_ctx, cmd->reg, cmd->val);
		if (cmd->delay_ms)
			mipi_dsi_msleep(&dsi_ctx, cmd->delay_ms);
	}

	/* Vendor init_lcm_registers(): Sleep Out, then 120 ms. */
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	nt51021_cmd_window(&dsi_ctx, false);
	mipi_dsi_msleep(&dsi_ctx, 5);

	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_disable(ctx->supply);
	}

	return dsi_ctx.accum_err;
}

static int nt51021_enable(struct drm_panel *panel)
{
	struct nt51021 *ctx = to_nt51021(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	nt51021_cmd_window(&dsi_ctx, true);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	nt51021_cmd_window(&dsi_ctx, false);
	mipi_dsi_msleep(&dsi_ctx, 20);

	return dsi_ctx.accum_err;
}

static int nt51021_disable(struct drm_panel *panel)
{
	struct nt51021 *ctx = to_nt51021(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	nt51021_cmd_window(&dsi_ctx, true);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	nt51021_cmd_window(&dsi_ctx, false);

	return dsi_ctx.accum_err;
}

static int nt51021_unprepare(struct drm_panel *panel)
{
	struct nt51021 *ctx = to_nt51021(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* Vendor lcm_suspend(): force MIPI, Sleep In, 50 ms. */
	nt51021_cmd_window(&dsi_ctx, true);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 50);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_disable(ctx->supply);

	return dsi_ctx.accum_err;
}

/*
 * Timings from the vendor's lcm_get_params(): 1200x1920, hsync 1 / hbp 32 /
 * hfp 110, vsync 1 / vbp 14 / vfp 11. That gives htotal 1343 and vtotal 1946.
 *
 * The pixel clock follows the vendor's PLL_CLOCK = 490, which after DDR is
 * 980 Mbps per lane, and over four lanes at 24bpp is 163.33 MHz, giving
 * 62.5 Hz. That is also what the bootloader leaves behind: read back before
 * the kernel touches it, the MIPI TX PLL on this board decodes to exactly
 * 980 Mbps per lane, so the panel is already running at this rate when the
 * kernel takes over.
 *
 * Note for anyone tempted by the "fps=6025" the bootloader puts on the kernel
 * command line: it is not the DSI rate and does not describe this PLL. It also
 * is not stable across boots.
 */
static const struct drm_display_mode nt51021_mode = {
	.clock = 163333,
	.hdisplay = 1200,
	.hsync_start = 1200 + 110,
	.hsync_end = 1200 + 110 + 1,
	.htotal = 1200 + 110 + 1 + 32,
	.vdisplay = 1920,
	.vsync_start = 1920 + 11,
	.vsync_end = 1920 + 11 + 1,
	.vtotal = 1920 + 11 + 1 + 14,
	.width_mm = 136,
	.height_mm = 221,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int nt51021_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &nt51021_mode);
}

static const struct drm_panel_funcs nt51021_panel_funcs = {
	.prepare = nt51021_prepare,
	.enable = nt51021_enable,
	.disable = nt51021_disable,
	.unprepare = nt51021_unprepare,
	.get_modes = nt51021_get_modes,
};

static int nt51021_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct nt51021 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct nt51021, panel,
				   &nt51021_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply))
		return dev_err_probe(dev, PTR_ERR(ctx->supply),
				     "failed to get power supply\n");

	/* Active low in the device tree, so OUT_HIGH leaves reset asserted. */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset GPIO\n");

	ctx->desc = of_device_get_match_data(dev);
	if (!ctx->desc)
		return -ENODEV;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/*
	 * Video mode with sync events rather than sync pulses
	 * (vendor SYNC_EVENT_VDO_MODE), and a continuously running HS clock
	 * (vendor cont_clock = 1), so MIPI_DSI_CLOCK_NON_CONTINUOUS is
	 * deliberately absent. Commands go out in LP.
	 */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get backlight\n");

	/* The DSI host must be running before we can send the init sequence. */
	ctx->panel.prepare_prev_first = true;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void nt51021_remove(struct mipi_dsi_device *dsi)
{
	struct nt51021 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id nt51021_of_match[] = {
	{ .compatible = "amazon,suez-boe-nt51021", .data = &suez_boe_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, nt51021_of_match);

static struct mipi_dsi_driver nt51021_driver = {
	.probe = nt51021_probe,
	.remove = nt51021_remove,
	.driver = {
		.name = "panel-novatek-nt51021",
		.of_match_table = nt51021_of_match,
	},
};
module_mipi_dsi_driver(nt51021_driver);

MODULE_DESCRIPTION("Novatek NT51021 1200x1920 DSI panel driver");
MODULE_LICENSE("GPL");
