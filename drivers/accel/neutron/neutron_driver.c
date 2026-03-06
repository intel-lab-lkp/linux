// SPDX-License-Identifier: GPL-2.0+
/* Copyright 2025-2026 NXP */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_gem.h>
#include <drm/neutron_accel.h>

#include "neutron_device.h"
#include "neutron_driver.h"
#include "neutron_gem.h"

#define NEUTRON_SUSPEND_DELAY_MS 1000

static const struct drm_ioctl_desc neutron_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(NEUTRON_CREATE_BO, neutron_ioctl_create_bo, 0),
	DRM_IOCTL_DEF_DRV(NEUTRON_SYNC_BO, neutron_ioctl_sync_bo, 0),
};

static int neutron_open(struct drm_device *drm, struct drm_file *file)
{
	struct neutron_device *ndev = to_neutron_device(drm);
	struct neutron_file_priv *npriv;

	npriv = kzalloc_obj(*npriv);
	if (!npriv)
		return -ENOMEM;

	npriv->ndev = ndev;
	file->driver_priv = npriv;

	return 0;
}

static void neutron_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct neutron_file_priv *npriv = file->driver_priv;

	kfree(npriv);
}

DEFINE_DRM_ACCEL_FOPS(neutron_drm_driver_fops);

static const struct drm_driver neutron_drm_driver = {
	.driver_features	= DRIVER_COMPUTE_ACCEL | DRIVER_GEM,
	.name			= "neutron",
	.desc			= "NXP Neutron driver",
	.major			= 1,
	.minor			= 0,

	.fops			= &neutron_drm_driver_fops,
	.open			= neutron_open,
	.postclose		= neutron_postclose,
	.ioctls			= neutron_drm_ioctls,
	.num_ioctls		= ARRAY_SIZE(neutron_drm_ioctls),

	.gem_create_object      = neutron_gem_create_object,
};

static irqreturn_t neutron_irq_handler_thread(int irq, void *data)
{
	struct neutron_device *ndev = data;

	neutron_handle_irq(ndev);

	return IRQ_HANDLED;
}

static int neutron_map_region(struct platform_device *pdev, char *name,
			      enum neutron_mem_id id)
{
	struct neutron_device *ndev = platform_get_drvdata(pdev);
	struct neutron_mem_region *mem = &ndev->mem_regions[id];
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return -EINVAL;

	mem->va = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mem->va))
		return PTR_ERR(mem->va);

	mem->size = resource_size(res);

	if (id == NEUTRON_MEM_ITCM)
		mem->da = NEUTRON_ITCM_DA;
	else if (id == NEUTRON_MEM_DTCM)
		mem->da = NEUTRON_DTCM_DA;

	return 0;
}

static int neutron_probe(struct platform_device *pdev)
{
	struct neutron_device *ndev;
	struct device *dev;
	int ret;

	ndev = devm_drm_dev_alloc(&pdev->dev, &neutron_drm_driver,
				  struct neutron_device, base);
	if (IS_ERR(ndev))
		return PTR_ERR(ndev);

	platform_set_drvdata(pdev, ndev);
	dev = &pdev->dev;
	ndev->dev = dev;

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48));

	/* Map registers, ITCM and DTCM regions of the Neutron device */
	ret = neutron_map_region(pdev, "regs", NEUTRON_MEM_REGS);
	if (ret)
		return ret;
	ret = neutron_map_region(pdev, "itcm", NEUTRON_MEM_ITCM);
	if (ret)
		return ret;
	ret = neutron_map_region(pdev, "dtcm", NEUTRON_MEM_DTCM);
	if (ret)
		return ret;

	ndev->num_clks = devm_clk_bulk_get_all(dev, &ndev->clks);
	if (ndev->num_clks < 0)
		return ndev->num_clks;

	ndev->irq = platform_get_irq(pdev, 0);
	if (ndev->irq < 0)
		return ndev->irq;

	ret = devm_request_threaded_irq(dev, ndev->irq, NULL,
					neutron_irq_handler_thread,
					IRQF_ONESHOT, KBUILD_MODNAME, ndev);
	if (ret) {
		dev_err(dev, "Failed to request irq %d\n", ndev->irq);
		return ret;
	}

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret) {
		dev_err(dev, "Failed to initialize reserved memory\n");
		return ret;
	}

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		goto free_reserved;

	pm_runtime_set_autosuspend_delay(dev, NEUTRON_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);

	ret = drm_dev_register(&ndev->base, 0);
	if (ret)
		goto free_reserved;

	return 0;

free_reserved:
	of_reserved_mem_device_release(&pdev->dev);

	return ret;
}

static void neutron_remove(struct platform_device *pdev)
{
	struct neutron_device *ndev = platform_get_drvdata(pdev);

	drm_dev_unregister(&ndev->base);
	of_reserved_mem_device_release(&pdev->dev);
}

static int neutron_runtime_suspend(struct device *dev)
{
	struct neutron_device *ndev = dev_get_drvdata(dev);

	neutron_disable_irq(ndev);
	neutron_shutdown(ndev);

	clk_bulk_disable_unprepare(ndev->num_clks, ndev->clks);

	return 0;
}

static int neutron_runtime_resume(struct device *dev)
{
	struct neutron_device *ndev = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(ndev->num_clks, ndev->clks);
	if (ret)
		return ret;

	ret = neutron_boot(ndev);
	if (ret) {
		clk_bulk_disable_unprepare(ndev->num_clks, ndev->clks);
		return ret;
	}

	neutron_enable_irq(ndev);

	return 0;
}

static const struct dev_pm_ops neutron_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	RUNTIME_PM_OPS(neutron_runtime_suspend, neutron_runtime_resume, NULL)
};

static const struct of_device_id neutron_match_table[] = {
	{ .compatible = "nxp,imx95-neutron" },
	{}
};

MODULE_DEVICE_TABLE(of, neutron_match_table);

static struct platform_driver neutron_driver = {
	.probe	= &neutron_probe,
	.remove	= &neutron_remove,
	.driver	= {
		.name		= "neutron",
		.of_match_table	= of_match_ptr(neutron_match_table),
		.pm		= pm_ptr(&neutron_pm_ops),
	},
};
module_platform_driver(neutron_driver);

MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("NXP Neutron Accel Driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(NEUTRON_FIRMWARE_NAME);
