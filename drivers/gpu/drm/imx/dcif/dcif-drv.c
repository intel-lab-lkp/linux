// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright 2025 NXP
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_print.h>

#include "dcif-drv.h"
#include "dcif-reg.h"

#define DCIF_CPU_DOMAIN			0

DEFINE_DRM_GEM_DMA_FOPS(dcif_driver_fops);

static struct drm_driver dcif_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.fops			= &dcif_driver_fops,
	.name			= "imx-dcif",
	.desc			= "i.MX DCIF DRM graphics",
	.major			= 1,
	.minor			= 0,
	.patchlevel		= 0,
};

static void dcif_read_chip_info(struct dcif_dev *dcif)
{
	struct drm_device *drm = &dcif->drm;
	u32 val, vmin, vmaj;
	int ret;

	ret = pm_runtime_resume_and_get(drm->dev);
	if (ret < 0) {
		drm_err(drm, "failed to resume DCIF: %d\n", ret);
		return;
	}

	regmap_read(dcif->regmap, DCIF_VER, &val);

	dcif->has_crc = val & DCIF_FEATURE_CRC;

	vmin = DCIF_VER_GET_MINOR(val);
	vmaj = DCIF_VER_GET_MAJOR(val);
	DRM_DEV_DEBUG(drm->dev, "DCIF version is %d.%d\n", vmaj, vmin);

	pm_runtime_put_sync(drm->dev);
}

static const struct regmap_config dcif_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.fast_io = true,
	.max_register = 0x20250,
	.cache_type = REGCACHE_NONE,
	.disable_locking = true,
};

static int dcif_probe(struct platform_device *pdev)
{
	struct dcif_dev *dcif;
	struct drm_device *drm;
	int ret;
	int i;

	dcif = devm_drm_dev_alloc(&pdev->dev, &dcif_driver, struct dcif_dev, drm);
	if (IS_ERR(dcif))
		return PTR_ERR(dcif);

	/* CPU 0 domain for interrupt control */
	dcif->cpu_domain = DCIF_CPU_DOMAIN;

	drm = &dcif->drm;
	dev_set_drvdata(&pdev->dev, dcif);

	dcif->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dcif->reg_base))
		return dev_err_probe(drm->dev, PTR_ERR(dcif->reg_base),
				     "failed to get reg base\n");

	for (i = 0; i < 3; i++) {
		dcif->irq[i] = platform_get_irq(pdev, i);
		if (dcif->irq[i] < 0)
			return dev_err_probe(drm->dev, dcif->irq[i],
					     "failed to get domain%d irq\n", i);
	}

	dcif->regmap = devm_regmap_init_mmio(drm->dev, dcif->reg_base, &dcif_regmap_config);
	if (IS_ERR(dcif->regmap))
		return dev_err_probe(drm->dev, PTR_ERR(dcif->regmap),
				     "failed to init DCIF regmap\n");

	dcif->num_clks = devm_clk_bulk_get_all(drm->dev, &dcif->clks);
	if (dcif->num_clks < 0)
		return dev_err_probe(drm->dev, dcif->num_clks,
				     "cannot get required clocks\n");

	dma_set_mask_and_coherent(drm->dev, DMA_BIT_MASK(32));

	devm_pm_runtime_enable(drm->dev);

	ret = devm_request_irq(drm->dev, dcif->irq[dcif->cpu_domain],
			       dcif_irq_handler, 0, drm->driver->name, drm);
	if (ret < 0)
		return dev_err_probe(drm->dev, ret, "failed to install IRQ handler\n");

	dcif_read_chip_info(dcif);

	ret = dcif_kms_prepare(dcif);
	if (ret)
		return ret;

	ret = drm_dev_register(drm, 0);
	if (ret)
		return dev_err_probe(drm->dev, ret, "failed to register drm device\n");

	drm_client_setup(drm, NULL);

	return 0;
}

static void dcif_remove(struct platform_device *pdev)
{
	struct dcif_dev *dcif = dev_get_drvdata(&pdev->dev);
	struct drm_device *drm = &dcif->drm;

	drm_dev_unregister(drm);

	drm_atomic_helper_shutdown(drm);
}

static void dcif_shutdown(struct platform_device *pdev)
{
	struct dcif_dev *dcif = dev_get_drvdata(&pdev->dev);
	struct drm_device *drm = &dcif->drm;

	drm_atomic_helper_shutdown(drm);
}

static int dcif_runtime_suspend(struct device *dev)
{
	struct dcif_dev *dcif = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(dcif->num_clks, dcif->clks);

	return 0;
}

static int dcif_runtime_resume(struct device *dev)
{
	struct dcif_dev *dcif = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(dcif->num_clks, dcif->clks);
	if (ret) {
		dev_err(dev, "failed to enable clocks: %d\n", ret);
		return ret;
	}

	return 0;
}

static int dcif_suspend(struct device *dev)
{
	struct dcif_dev *dcif = dev_get_drvdata(dev);
	int ret;

	ret = drm_mode_config_helper_suspend(&dcif->drm);
	if (ret < 0)
		return ret;

	if (pm_runtime_suspended(dev))
		return 0;

	return dcif_runtime_suspend(dev);
}

static int dcif_resume(struct device *dev)
{
	struct dcif_dev *dcif = dev_get_drvdata(dev);
	int ret;

	if (!pm_runtime_suspended(dev)) {
		ret = dcif_runtime_resume(dev);
		if (ret < 0)
			return ret;
	}

	return drm_mode_config_helper_resume(&dcif->drm);
}

static const struct dev_pm_ops dcif_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dcif_suspend, dcif_resume)
	SET_RUNTIME_PM_OPS(dcif_runtime_suspend, dcif_runtime_resume, NULL)
};

static const struct of_device_id dcif_dt_ids[] = {
	{ .compatible = "nxp,imx94-dcif", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, dcif_dt_ids);

static struct platform_driver dcif_platform_driver = {
	.probe	= dcif_probe,
	.remove	= dcif_remove,
	.shutdown = dcif_shutdown,
	.driver	= {
		.name		= "imx-dcif-drm",
		.of_match_table	= dcif_dt_ids,
		.pm		= pm_ptr(&dcif_pm_ops),
	},
};
module_platform_driver(dcif_platform_driver);

MODULE_AUTHOR("NXP Semiconductor");
MODULE_DESCRIPTION("i.MX94 DCIF DRM driver");
MODULE_LICENSE("GPL");
