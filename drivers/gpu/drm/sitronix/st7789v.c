// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for Sitronix ST7789V LCD panels
 *
 * Copyright (C) 2026 Archit Anant <architanant5@gmail.com>
 */

#include <linux/bits.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/backlight.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modes.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_fbdev_dma.h>
#include <video/mipi_display.h>

#define ST7789V_PORCTRL      0xb2
#define ST7789V_GCTRL        0xb7
#define ST7789V_VCOMS        0xbb
#define ST7789V_LCMCTRL      0xc0
#define ST7789V_VDVVRHEN     0xc2
#define ST7789V_VRHS         0xc3
#define ST7789V_VDVS         0xc4
#define ST7789V_VCMOFSET     0xc5
#define ST7789V_FRCTRL2      0xc6
#define ST7789V_PWCTRL1      0xd0
#define ST7789V_PVGAMCTRL    0xe0
#define ST7789V_NVGAMCTRL    0xe1

#define ST7789V_MADCTL_MY  BIT(7)
#define ST7789V_MADCTL_MX  BIT(6)
#define ST7789V_MADCTL_MV  BIT(5)
#define ST7789V_MADCTL_BGR BIT(3)


struct st7789v_cfg {
	const struct drm_display_mode mode;
	unsigned int left_offset;
	unsigned int top_offset;
	bool is_ips;   /* Controls PORCTRL and GCTRL timings */
	bool invert;   /* Controls Color Inversion (positive/negative) */
};
struct st7789v_priv {
	struct mipi_dbi_dev dbidev; /* Must be first for .release() */
	const struct st7789v_cfg *cfg;
};


/* 1. Generic Fallback (Matches default behavior of fb_st7789v.c) */
static const struct st7789v_cfg generic_cfg = {
	.mode = { DRM_SIMPLE_MODE(240, 320, 0, 0) },
	.is_ips = false,
	.invert = true,
};

/* 2. HannStar 2.0" IPS (The specific panel handled in staging) */
static const struct st7789v_cfg hsd20_ips_cfg = {
	.mode = { DRM_SIMPLE_MODE(240, 320, 31, 41) },
	.is_ips = true,
	.invert = true,
};

/* 3. Inanbo 2.8" (From the 9-bit driver: No Inversion) */
static const struct st7789v_cfg inanbo_panel_cfg = {
	.mode = { DRM_SIMPLE_MODE(240, 320, 43, 57) },
	.is_ips = false,
	.invert = false,
};

/* 4. EDT 2.8" (From the 9-bit driver: Normal Inversion) */
static const struct st7789v_cfg edt_panel_cfg = {
	.mode = { DRM_SIMPLE_MODE(240, 320, 43, 58) },
	.is_ips = false,
	.invert = true,
};

/* 5. Jasonic 2.4" (From the 9-bit driver: Custom Height + Offset) */
static const struct st7789v_cfg jasonic_panel_cfg = {
	.mode = { DRM_SIMPLE_MODE(240, 280, 37, 43) },
	.is_ips = true,
	.invert = true,
	.top_offset = 38,
};

DEFINE_DRM_GEM_DMA_FOPS(st7789v_fops);

static const struct drm_driver st7789v_driver = {
    .driver_features    = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
    .fops               = &st7789v_fops,
    DRM_GEM_DMA_DRIVER_OPS_VMAP,
    DRM_FBDEV_DMA_DRIVER_OPS,
    .debugfs_init       = mipi_dbi_debugfs_init,
    .name               = "st7789v",
    .desc               = "Sitronix ST7789V",
    .major              = 1,
    .minor              = 0,
};

static void st7789v_pipe_enable(struct drm_simple_display_pipe *pipe,
                                struct drm_crtc_state *crtc_state,
                                struct drm_plane_state *plane_state)
{
    struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
    struct st7789v_priv *priv = container_of(dbidev, struct st7789v_priv, dbidev);
    struct mipi_dbi *dbi = &dbidev->dbi;
    int ret,idx; 

    if (!drm_dev_enter(pipe->crtc.dev, &idx))
        return;

    ret = mipi_dbi_poweron_reset(dbidev); 
    if (ret)
		goto out_exit;

    mipi_dbi_command(dbi, MIPI_DCS_SOFT_RESET);
    msleep(150);

    mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
    msleep(500);

    mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, MIPI_DCS_PIXEL_FMT_16BIT);

    if (priv->cfg->is_ips) {
        mipi_dbi_command(dbi, ST7789V_PORCTRL, 0x05, 0x05, 0x00, 0x33, 0x33);
        mipi_dbi_command(dbi, ST7789V_GCTRL, 0x75);
    } else {
        mipi_dbi_command(dbi, ST7789V_PORCTRL, 0x0c, 0x0c, 0x00, 0x33, 0x33);
        mipi_dbi_command(dbi, ST7789V_GCTRL, 0x35);
    }

    mipi_dbi_command(dbi, ST7789V_VCOMS, 0x20); 
    mipi_dbi_command(dbi, ST7789V_LCMCTRL, 0x2c);
    mipi_dbi_command(dbi, ST7789V_VDVVRHEN, 0x01);
    mipi_dbi_command(dbi, ST7789V_VRHS, 0x12);
    mipi_dbi_command(dbi, ST7789V_VDVS, 0x20);
    mipi_dbi_command(dbi, ST7789V_FRCTRL2, 0x0f);
    mipi_dbi_command(dbi, ST7789V_PWCTRL1, 0xa4, 0xa1);

    mipi_dbi_command(dbi, ST7789V_PVGAMCTRL, 
                     0xd0, 0x04, 0x0d, 0x11, 0x13, 0x2b, 0x3f, 0x54, 
                     0x4c, 0x18, 0x0d, 0x0b, 0x1f, 0x23);
    mipi_dbi_command(dbi, ST7789V_NVGAMCTRL, 
                     0xd0, 0x04, 0x0c, 0x11, 0x13, 0x2c, 0x3f, 0x44, 
                     0x51, 0x2f, 0x1f, 0x1f, 0x20, 0x23);

    if (priv->cfg->invert)
        mipi_dbi_command(dbi, MIPI_DCS_ENTER_INVERT_MODE);
    else
        mipi_dbi_command(dbi, MIPI_DCS_EXIT_INVERT_MODE);
    mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
    msleep(100);

    u8 addr_mode = 0;

    switch (dbidev->rotation) {
    case 90:

        addr_mode = ST7789V_MADCTL_MV | ST7789V_MADCTL_MY;
        break;
    case 180:
        addr_mode = ST7789V_MADCTL_MX | ST7789V_MADCTL_MY;
        break;
    case 270:
        addr_mode = ST7789V_MADCTL_MV | ST7789V_MADCTL_MX;
        break;
    default: 
        addr_mode = 0;
        break;
    }

    addr_mode |= ST7789V_MADCTL_BGR; 

    mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

    mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);

out_exit:
    drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs st7789v_pipe_funcs = 
{
    DRM_MIPI_DBI_SIMPLE_DISPLAY_PIPE_FUNCS(st7789v_pipe_enable),
};

static int st7789v_probe(struct spi_device *spi)
{
    struct device *dev = &spi->dev;
    const struct st7789v_cfg *cfg;
    struct mipi_dbi_dev *dbidev;
    struct st7789v_priv *priv;
    struct drm_device *drm;
    struct mipi_dbi *dbi;
    struct gpio_desc *dc;
    u32 rotation = 0;
    int ret;

    cfg = device_get_match_data(&spi->dev);

	if (!cfg)
		cfg = (void *)spi_get_device_id(spi)->driver_data;

	priv = devm_drm_dev_alloc(dev, &st7789v_driver,
				  struct st7789v_priv, dbidev.drm);

	if (IS_ERR(priv))
		return PTR_ERR(priv);

    dbidev = &priv->dbidev;
    priv->cfg = cfg;

    dbi = &dbidev->dbi;
    drm = &dbidev->drm;

    spi_set_drvdata(spi, drm);

    dbi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(dbi->reset))
        return dev_err_probe(dev, PTR_ERR(dbi->reset), "Failed to get GPIO 'reset'\n");

    dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
    if (IS_ERR(dc))
        return dev_err_probe(dev, PTR_ERR(dc), "Failed to get GPIO 'dc'\n");

    dbidev->backlight = devm_of_find_backlight(dev);
    if (IS_ERR(dbidev->backlight))
        return PTR_ERR(dbidev->backlight);
    
    dbidev->left_offset = priv->cfg->left_offset;
    dbidev->top_offset = priv->cfg->top_offset;
    
    device_property_read_u32(dev, "rotation", &rotation);

    ret = mipi_dbi_spi_init(spi, dbi, dc);
    if (ret)
        return ret;
    
    ret = mipi_dbi_dev_init(dbidev, &st7789v_pipe_funcs, &cfg->mode, rotation);
    if (ret)
        return ret;

    drm_mode_config_reset(drm);
    
    ret = drm_dev_register(drm, 0);
    if (ret)
        return ret;

    drm_client_setup(drm, NULL);

    return 0;
}

static void st7789v_remove(struct spi_device *spi)
{
    struct drm_device *drm = spi_get_drvdata(spi);
    drm_dev_unplug(drm);
    drm_atomic_helper_shutdown(drm);
}

static void st7789v_shutdown(struct spi_device *spi)
{
    drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}


static const struct of_device_id st7789v_of_match[] = {
	{ .compatible = "sitronix,st7789v", .data = &generic_cfg },
	{ .compatible = "hannstar,hsd20-ips", .data = &hsd20_ips_cfg },
	{ .compatible = "inanbo,t28cp45tn89-v17", .data = &inanbo_panel_cfg },
	{ .compatible = "edt,et028013dma", .data = &edt_panel_cfg },
	{ .compatible = "jasonic,jt240mhqs-hwt-ek-e3", .data = &jasonic_panel_cfg },
	{  }
};
MODULE_DEVICE_TABLE(of, st7789v_of_match);

static const struct spi_device_id st7789v_id[] = {
    { "st7789v", 0 },
    { },
};
MODULE_DEVICE_TABLE(spi, st7789v_id);

static struct spi_driver st7789v_spi_driver = {
    .driver = {
        .name = "st7789v",
        .of_match_table = st7789v_of_match,
    },
    .probe = st7789v_probe,
    .remove = st7789v_remove,
    .shutdown = st7789v_shutdown,
    .id_table = st7789v_id,
};

module_spi_driver(st7789v_spi_driver);

MODULE_DESCRIPTION("Sitronix ST7789V DRM driver");
MODULE_AUTHOR("Archit Anant <architanant5@gmail.com>");
MODULE_LICENSE("GPL");