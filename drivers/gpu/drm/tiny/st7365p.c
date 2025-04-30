// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for display panels connected to Sitronix ST7365P
 * display controller in SPI mode.
 *
 * Copyright (C) Braiins Systems s.r.o. 2025
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>
#include <video/mipi_display.h>

/* ST7356P specific defines */
#define ST7356P_DIC     0xB4 // Display Inversion Control
#define ST7356P_EM      0xB7 // Entry Mode Set
#define ST7356P_PWR1    0xC0 // Power Control 1
#define ST7356P_PWR2    0xC1 // Power Control 2
#define ST7356P_PWR3    0xC2 // Power Control 3
#define ST7356P_VCMPCTL 0xC5 // VCOM Control
#define ST7356P_VCMOST  0xC6 // VCOM Offset Register
#define ST7356P_PGC     0xE0 // Positive Gamma Control
#define ST7356P_NGC     0xE1 // Negative Gamma Control
#define ST7356P_DOCA    0xE8 // Display Output Ctrl Adjust
#define ST7356P_CSCON   0xF0 // Command Set Control

#define ST7356P_CSCON_ENABLE_PART_1  0xC3
#define ST7356P_CSCON_ENABLE_PART_2  0x96
#define ST7356P_CSCON_DISABLE_PART_1 0x3C
#define ST7356P_CSCON_DISABLE_PART_2 0x69

#define MADCTL_MY    BIT(7) // Row Address Order
#define MADCTL_MX    BIT(6) // Column Address Order
#define MADCTL_MV    BIT(5) // Row/Column Exchange
#define MADCTL_ML    BIT(4) // Vertical Refresh Order
#define MADCTL_BGR   BIT(3) // RGB-BGR ORDER

struct st7365p_cfg {
	const struct drm_display_mode mode;
	unsigned int inverted:1; /* Color invert mode */
};

struct st7365p_priv {
	struct mipi_dbi_dev dbidev;	/* Must be first for .release() */
	const struct st7365p_cfg *cfg;
};

static void st7365p_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct st7365p_priv *priv = container_of(dbidev, struct st7365p_priv,
						 dbidev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	int ret, idx;
	u8 addr_mode;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	DRM_DEBUG_KMS("\n");

	ret = mipi_dbi_poweron_reset(dbidev);
	if (ret)
		goto out_exit;

	/* Exit sleep mode */
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(5);

	/* 16-bit pixels */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, MIPI_DCS_PIXEL_FMT_16BIT);

	/* ST7365P specific settings */
	mipi_dbi_command(dbi, ST7356P_CSCON, ST7356P_CSCON_ENABLE_PART_1);
	mipi_dbi_command(dbi, ST7356P_CSCON, ST7356P_CSCON_ENABLE_PART_2);

	mipi_dbi_command(dbi, ST7356P_DIC, 0x0001);
	mipi_dbi_command(dbi, ST7356P_EM, 0x00C6);

	mipi_dbi_command(dbi, ST7356P_PWR2, 0x0015);
	mipi_dbi_command(dbi, ST7356P_PWR3, 0x00AF);
	mipi_dbi_command(dbi, ST7356P_VCMPCTL, 0x0022);
	mipi_dbi_command(dbi, ST7356P_VCMOST, 0x0000);
	mipi_dbi_command(dbi, ST7356P_DOCA, 0x0040, 0x008A, 0x0000, 0x0000,
					    0x0029, 0x0019, 0x00A5, 0x0033);

	mipi_dbi_command(dbi, ST7356P_CSCON, ST7356P_CSCON_DISABLE_PART_1);
	mipi_dbi_command(dbi, ST7356P_CSCON, ST7356P_CSCON_DISABLE_PART_2);

	/* Enter inverted mode */
	if (priv->cfg->inverted)
		mipi_dbi_command(dbi, MIPI_DCS_ENTER_INVERT_MODE);

	/* Rotation */
	addr_mode = 0;
	switch (dbidev->rotation) {
	case 0:
		addr_mode |= (MADCTL_MX);
		break;
	case 90:
		addr_mode |= (MADCTL_MV | MADCTL_MX | MADCTL_MY);
		break;
	case 180:
		addr_mode |= (MADCTL_MY);
		break;
	case 270:
		addr_mode |= (MADCTL_MV);
		break;
	default:
		addr_mode = 0;
	}

	/* NOTE: The meaning of this attribute has opposite effect on
	 * the controller so 'MADCTL_BGR' is used for RGB mode.
	 */
	addr_mode |= MADCTL_BGR;

	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	/* Turn on Brightness Control Block and Backlight Control */
	mipi_dbi_command(dbi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);

	/* Turn on the display */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);

	mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);

out_exit:
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs st7365p_pipe_funcs = {
	DRM_MIPI_DBI_SIMPLE_DISPLAY_PIPE_FUNCS(st7365p_pipe_enable),
};

static const struct st7365p_cfg kingway_hw035p0z002 = {
	.mode       = { DRM_SIMPLE_MODE(320, 480, 49, 73) },
	.inverted   = 1,
};

DEFINE_DRM_GEM_DMA_FOPS(st7365p_fops);

static const struct drm_driver st7365p_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops            = &st7365p_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.debugfs_init    = mipi_dbi_debugfs_init,
	.name            = "st7365p",
	.desc            = "Sitronix ST7365P",
	.major           = 1,
	.minor           = 0,
};

static const struct of_device_id st7365p_of_match[] = {
	{ .compatible = "kingway,hw-035p0z002", .data = &kingway_hw035p0z002 },
	{ },
};
MODULE_DEVICE_TABLE(of, st7365p_of_match);

static const struct spi_device_id st7365p_id[] = {
	{ "hw-035p0z002", (uintptr_t)&kingway_hw035p0z002 },
	{ },
};
MODULE_DEVICE_TABLE(spi, st7365p_id);

static int st7365p_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	const struct st7365p_cfg *cfg;
	struct mipi_dbi_dev *dbidev;
	struct st7365p_priv *priv;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	u32 rotation;
	int ret;

	cfg = device_get_match_data(&spi->dev);
	if (!cfg)
		cfg = (void *)spi_get_device_id(spi)->driver_data;

	priv = devm_drm_dev_alloc(dev, &st7365p_driver,
				  struct st7365p_priv, dbidev.drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	dbidev = &priv->dbidev;
	priv->cfg = cfg;

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;

	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	dbi->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset))
		return dev_err_probe(dev, PTR_ERR(dbi->reset), "Failed to get GPIO 'reset'\n");

	dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc))
		return dev_err_probe(dev, PTR_ERR(dc), "Failed to get GPIO 'dc'\n");

	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret)
		return ret;

	/* Disable reading from display */
	dbi->read_commands = NULL;

	rotation = 0;
	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_dev_init(dbidev, &st7365p_pipe_funcs, &cfg->mode, rotation);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	drm_client_setup(drm, NULL);

	return 0;
}

static void st7365p_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void st7365p_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver st7365p_spi_driver = {
	.driver = {
		.name = "st7365p",
		.of_match_table = st7365p_of_match,
	},
	.id_table = st7365p_id,
	.probe = st7365p_probe,
	.remove = st7365p_remove,
	.shutdown = st7365p_shutdown,
};
module_spi_driver(st7365p_spi_driver);

MODULE_DESCRIPTION("Sitronix ST7365P DRM driver");
MODULE_AUTHOR("Josef Lusticky <josef.lusticky@braiins.cz>");
MODULE_LICENSE("GPL");
