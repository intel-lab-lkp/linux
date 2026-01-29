// SPDX-License-Identifier: GPL-2.0+
/*
 * Nuvoton DRM driver
 *
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <a0987203069@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "ma35_drm.h"

DEFINE_DRM_GEM_DMA_FOPS(ma35_drm_fops);

static int ma35_drm_gem_dma_dumb_create(struct drm_file *file_priv,
					   struct drm_device *drm_dev,
					   struct drm_mode_create_dumb *args)
{
	struct drm_mode_config *mode_config = &drm_dev->mode_config;
	u32 pixel_align;

	if (args->width < mode_config->min_width ||
		args->height < mode_config->min_height)
		return -EINVAL;

	/* check for alignment */
	pixel_align = MA35_DISPLAY_ALIGN_PIXELS * args->bpp / 8;
	args->pitch = ALIGN(args->width * args->bpp / 8, pixel_align);

	return drm_gem_dma_dumb_create_internal(file_priv, drm_dev, args);
}

static struct drm_driver ma35_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC |
						  DRIVER_CURSOR_HOTSPOT,

	.fops				= &ma35_drm_fops,
	.name				= "ma35-drm",
	.desc				= "Nuvoton MA35 series DRM driver",
	.major				= DRIVER_MAJOR,
	.minor				= DRIVER_MINOR,

	DRM_GEM_DMA_DRIVER_OPS_VMAP_WITH_DUMB_CREATE(ma35_drm_gem_dma_dumb_create),
};

static const struct regmap_config ma35_drm_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= 0x2000,
	.name		= "ma35-drm",
};

static irqreturn_t ma35_drm_irq_handler(int irq, void *data)
{
	struct ma35_drm *priv = data;
	irqreturn_t ret = IRQ_NONE;
	u32 stat = 0;

	/* Get pending interrupt sources (RO) */
	regmap_read(priv->regmap, MA35_INT_STATE, &stat);

	if (stat & MA35_INT_STATE_DISP0) {
		ma35_crtc_vblank_handler(priv);
		ret = IRQ_HANDLED;
	}

	return ret;
}

static const struct drm_mode_config_funcs ma35_mode_config_funcs = {
	.fb_create		= drm_gem_fb_create,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs ma35_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail,
};

static int ma35_mode_init(struct ma35_drm *priv)
{
	struct drm_device *drm_dev = &priv->drm_dev;
	struct drm_mode_config *mode_config = &drm_dev->mode_config;
	int ret;

	ret = drmm_mode_config_init(drm_dev);
	if (ret) {
		drm_err(drm_dev, "Failed to init mode config\n");
		return -EINVAL;
	}

	drm_dev->max_vblank_count = MA35_DEBUG_COUNTER_MASK;
	ret = drm_vblank_init(drm_dev, 1);
	if (ret) {
		drm_err(drm_dev, "Failed to initialize vblank\n");
		return ret;
	}

	mode_config->min_width = 32;
	mode_config->max_width = 1920;
	mode_config->min_height = 1;
	mode_config->max_height = 1080;
	mode_config->preferred_depth = 24;
	mode_config->cursor_width = MA35_CURSOR_WIDTH;
	mode_config->cursor_height = MA35_CURSOR_HEIGHT;
	mode_config->funcs = &ma35_mode_config_funcs;
	mode_config->helper_private = &ma35_mode_config_helper_funcs;

	return 0;
}

static void ma35_mode_fini(struct ma35_drm *priv)
{
	struct drm_device *drm_dev = &priv->drm_dev;

	drm_kms_helper_poll_fini(drm_dev);
}

static int ma35_clocks_prepare(struct ma35_drm *priv)
{
	struct drm_device *drm_dev = &priv->drm_dev;
	struct device *dev = drm_dev->dev;
	int ret;

	priv->dcuclk = devm_clk_get(dev, "dcu_gate");
	if (IS_ERR(priv->dcuclk)) {
		dev_err(dev, "Failed to get display core clock\n");
		return PTR_ERR(priv->dcuclk);
	}

	ret = clk_prepare_enable(priv->dcuclk);
	if (ret) {
		dev_err(dev, "Failed to enable display core clock\n");
		return ret;
	}

	priv->dcupclk = devm_clk_get(dev, "dcup_div");
	if (IS_ERR(priv->dcupclk)) {
		dev_err(dev, "Failed to get display pixel clock\n");
		return PTR_ERR(priv->dcupclk);
	}

	ret = clk_prepare_enable(priv->dcupclk);
	if (ret) {
		dev_err(dev, "Failed to enable display pixel clock\n");
		return ret;
	}

	return 0;
}

static int ma35_clocks_unprepare(struct ma35_drm *priv)
{
	struct clk **clocks[] = {
		&priv->dcuclk,
		&priv->dcupclk,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(clocks); i++) {
		if (!*clocks[i])
			continue;

		clk_disable_unprepare(*clocks[i]);
		*clocks[i] = NULL;
	}

	return 0;
}

static int ma35_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ma35_drm *priv;
	struct drm_device *drm_dev;
	void __iomem *base;
	struct regmap *regmap = NULL;
	int irq;
	int ret;

	ret = of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV) {
		dev_err(dev, "Failed to get optional reserved memory: %d\n", ret);
		return ret;
	}

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		dev_err(dev, "Failed to map I/O base\n");
		ret = PTR_ERR(base);
		goto error_reserved_mem;
	}
	regmap = devm_regmap_init_mmio(dev, base, &ma35_drm_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "Failed to create regmap for I/O\n");
		ret = PTR_ERR(regmap);
		goto error_reserved_mem;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = -ENODEV;
		goto error_reserved_mem;
	}

	priv = devm_drm_dev_alloc(dev, &ma35_drm_driver,
				     struct ma35_drm, drm_dev);
	if (IS_ERR(priv)) {
		ret = PTR_ERR(priv);
		goto error_reserved_mem;
	}

	platform_set_drvdata(pdev, priv);
	drm_dev = &priv->drm_dev;
	priv->regmap = regmap;
	INIT_LIST_HEAD(&priv->layers_list);

	ret = ma35_clocks_prepare(priv);
	if (ret) {
		drm_err(drm_dev, "Failed to prepare clocks\n");
		goto error_reserved_mem;
	}

	ret = devm_request_irq(dev, irq, ma35_drm_irq_handler, 0,
			       dev_name(dev), priv);
	if (ret) {
		drm_err(drm_dev, "Failed to request IRQ\n");
		goto error_clocks;
	}

	/* modeset */
	ret = ma35_mode_init(priv);
	if (ret) {
		drm_err(drm_dev, "Failed to initialize KMS\n");
		goto error_clocks;
	}

	/* plane */
	ret = ma35_plane_init(priv);
	if (ret) {
		drm_err(drm_dev, "Failed to initialize layers\n");
		goto error_clocks;
	}

	/* crtc */
	ret = ma35_crtc_init(priv);
	if (ret) {
		drm_err(drm_dev, "Failed to initialize CRTC\n");
		goto error_clocks;
	}

	/* interface */
	ret = ma35_interface_init(priv);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			drm_err(drm_dev, "Failed to initialize interface\n");

		goto error_clocks;
	}

	drm_mode_config_reset(drm_dev);

	ret = drm_dev_register(drm_dev, 0);
	if (ret) {
		drm_err(drm_dev, "Failed to register DRM device\n");
		goto error_mode;
	}

	drm_client_setup(drm_dev, NULL);

	return 0;

error_mode:
	ma35_mode_fini(priv);

error_clocks:
	ma35_clocks_unprepare(priv);

error_reserved_mem:
	of_reserved_mem_device_release(dev);
	return ret;
}

static void ma35_drm_remove(struct platform_device *pdev)
{
	struct ma35_drm *priv = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	struct drm_device *drm_dev = &priv->drm_dev;

	drm_dev_unregister(drm_dev);
	drm_atomic_helper_shutdown(drm_dev);

	ma35_mode_fini(priv);

	ma35_clocks_unprepare(priv);

	of_reserved_mem_device_release(dev);
}

static void ma35_drm_shutdown(struct platform_device *pdev)
{
	struct ma35_drm *priv = platform_get_drvdata(pdev);
	struct drm_device *drm_dev = &priv->drm_dev;

	drm_atomic_helper_shutdown(drm_dev);
}

static __maybe_unused int ma35_drm_suspend(struct device *dev)
{
	struct ma35_drm *priv = dev_get_drvdata(dev);
	struct drm_device *drm_dev = &priv->drm_dev;

	return drm_mode_config_helper_suspend(drm_dev);
}

static __maybe_unused int ma35_drm_resume(struct device *dev)
{
	struct ma35_drm *priv = dev_get_drvdata(dev);
	struct drm_device *drm_dev = &priv->drm_dev;

	return drm_mode_config_helper_resume(drm_dev);
}

static const struct of_device_id ma35_drm_of_table[] = {
	{ .compatible = "nuvoton,ma35d1-dcu" },
	{},
};
MODULE_DEVICE_TABLE(of, ma35_drm_of_table);

static const struct dev_pm_ops ma35_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ma35_drm_suspend, ma35_drm_resume)
};

static struct platform_driver ma35_drm_platform_driver = {
	.probe		= ma35_drm_probe,
	.remove		= ma35_drm_remove,
	.shutdown	= ma35_drm_shutdown,
	.driver		= {
		.name		= "ma35-drm",
		.of_match_table	= ma35_drm_of_table,
		.pm = &ma35_pm_ops,
	},
};

module_platform_driver(ma35_drm_platform_driver);

MODULE_AUTHOR("Joey Lu <a0987203069@gmail.com>");
MODULE_DESCRIPTION("Nuvoton MA35 series DRM driver");
MODULE_LICENSE("GPL");
