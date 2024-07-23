// SPDX-License-Identifier: GPL-2.0+

/*
 * Authors:
 *      Sui Jingfeng <sui.jingfeng@linux.dev>
 */

#include <linux/component.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_aperture.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_ttm.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_probe_helper.h>

#include "loonggpu_pci_drv.h"
#include "loongson_drv.h"
#include "loongson_module.h"
#include "lsdc_drv.h"
#include "lsdc_gem.h"
#include "lsdc_output.h"
#include "lsdc_ttm.h"

DEFINE_DRM_GEM_FOPS(loongson_gem_fops);

static const struct drm_driver loongson_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_RENDER | DRIVER_GEM |
			   DRIVER_ATOMIC,
	.fops = &loongson_gem_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
	.debugfs_init = loongson_debugfs_init,
	.dumb_create = lsdc_dumb_create,
	.dumb_map_offset = lsdc_dumb_map_offset,
	.gem_prime_import_sg_table = lsdc_prime_import_sg_table,
};

/*
 * The GPU and display controller in the LS7A1000/LS7A2000/LS2K2000 are
 * separated PCIE devices. They are two devices, not one. Bar 2 of the GPU
 * device contains the base address and size of the VRAM, both the GPU and
 * the DC could access the on-board VRAM.
 */
static int loongson_drm_get_dedicated_vram(struct drm_device *drm)
{
	struct loongson_drm *ldrm = to_loongson_drm(drm);
	struct pci_dev *pdev_gpu;
	resource_size_t base, size;

	/*
	 * The GPU has 00:06.0 as its BDF, this is true at least for
	 * LS7A1000, LS7A2000 and LS2K2000.
	 */
	pdev_gpu = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(6, 0));
	if (!pdev_gpu) {
		drm_err(drm, "No GPU device, then no VRAM\n");
		return -ENODEV;
	}

	base = pci_resource_start(pdev_gpu, 2);
	size = pci_resource_len(pdev_gpu, 2);

	pci_dev_put(pdev_gpu);

	ldrm->vram_base = base;
	ldrm->vram_size = size;

	drm_info(drm, "Dedicated vram start: 0x%llx, size: %uMiB\n",
		 (u64)base, (u32)(size >> 20));

	return (size > SZ_1M) ? 0 : -ENODEV;
}

static int loongson_drm_master_bind(struct device *dev)
{
	struct loongson_drm *ldrm = dev_get_drvdata(dev);
	struct drm_device *ddev = &ldrm->ddev;
	int ret;

	loongson_drm_get_dedicated_vram(ddev);

	loongson_gfxpll_preinit(ddev);

	ret = drmm_mode_config_init(ddev);
	if (ret)
		return ret;

	ret = component_bind_all(dev, ddev);
	if (ret) {
		dev_err(dev, "master bind all failed: %d\n", ret);
		return ret;
	}

	drm_mode_config_reset(ddev);

	lsdc_gem_init(ddev);

	ret = lsdc_ttm_init(ddev);
	if (ret) {
		drm_err(ddev, "Memory manager init failed: %d\n", ret);
		return ret;
	}

	drmm_kms_helper_poll_init(ddev);

	ret = drm_dev_register(ddev, 0);
	if (ret)
		return ret;

	drm_fbdev_ttm_setup(ddev, 32);

	return 0;
}

static void loongson_drm_master_unbind(struct device *dev)
{
	struct loongson_drm *ldrm = dev_get_drvdata(dev);
	struct drm_device *ddev = &ldrm->ddev;

	drm_atomic_helper_shutdown(ddev);

	drm_dev_unregister(ddev);

	component_unbind_all(dev, ddev);
}

static const struct component_master_ops loongson_drm_master_ops = {
	.bind = loongson_drm_master_bind,
	.unbind = loongson_drm_master_unbind,
};

static int loongson_drm_freeze(struct drm_device *ddev)
{
	struct loongson_drm *ldrm = to_loongson_drm(ddev);
	struct lsdc_bo *lbo;
	int ret;

	/* unpin all of buffers in the VRAM */
	mutex_lock(&ldrm->gem.mutex);
	list_for_each_entry(lbo, &ldrm->gem.objects, list) {
		struct ttm_buffer_object *tbo = &lbo->tbo;
		struct ttm_resource *resource = tbo->resource;
		unsigned int pin_count = tbo->pin_count;

		drm_dbg(ddev, "bo[%p], size: %zuKiB, type: %s, pin count: %u\n",
			lbo, lsdc_bo_size(lbo) >> 10,
			lsdc_mem_type_to_str(resource->mem_type), pin_count);

		if (!pin_count)
			continue;

		if (resource->mem_type == TTM_PL_VRAM) {
			ret = lsdc_bo_reserve(lbo);
			if (unlikely(ret)) {
				drm_err(ddev, "bo reserve failed: %d\n", ret);
				continue;
			}

			do {
				lsdc_bo_unpin(lbo);
				--pin_count;
			} while (pin_count);

			lsdc_bo_unreserve(lbo);
		}
	}
	mutex_unlock(&ldrm->gem.mutex);

	lsdc_bo_evict_vram(ddev);

	ret = drm_mode_config_helper_suspend(ddev);
	if (unlikely(ret)) {
		drm_err(ddev, "Freeze error: %d", ret);
		return ret;
	}

	return 0;
}

static int loongson_drm_pm_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct loongson_drm *ldrm = platform_get_drvdata(pdev);

	return loongson_drm_freeze(&ldrm->ddev);
}

static int loongson_drm_pm_resume(struct platform_device *pdev)
{
	struct loongson_drm *ldrm = platform_get_drvdata(pdev);

	return drm_mode_config_helper_resume(&ldrm->ddev);
}

static void loongson_add_pci_match(struct device *master,
				   struct device_driver *driver,
				   struct component_match **matchptr)
{
	struct pci_driver *pdriver = to_pci_driver(driver);
	const struct pci_device_id *id_table = pdriver->id_table;
	unsigned int i = 0;
	struct pci_dev *pdev;

	while (id_table[i].vendor == PCI_VENDOR_ID_LOONGSON) {
		pdev = pci_get_device(PCI_VENDOR_ID_LOONGSON,
				      id_table[i].device,
				      NULL);
		if (!pdev) {
			++i;
			continue;
		}

		component_match_add(master, matchptr, component_compare_dev,
				    &pdev->dev);
		pci_dev_put(pdev);
		return;
	}
}

static void loongson_add_platform_match(struct device *master,
					struct device_driver *driver,
					struct component_match **matchptr)
{
	struct device *dev = NULL;

	while ((dev = platform_find_device_by_driver(dev, driver))) {
		component_match_add(master, matchptr, component_compare_dev, dev);
		put_device(dev);
	}
}

static void loongson_matches_add(struct device *master,
				 struct component_match **pptr)
{
	const struct loongson_driver_info *ldi;

	ldi = loongson_get_driver_info_array(NULL);
	while (ldi->driver) {
		if (ldi->type == LOONGSON_DRIVER_TYPE_PCI)
			loongson_add_pci_match(master, ldi->driver, pptr);
		else if (ldi->type == LOONGSON_DRIVER_TYPE_PLATFORM)
			loongson_add_platform_match(master, ldi->driver, pptr);

		++ldi;
	}
}

static int loongson_drm_driver_probe(struct platform_device *pdev)
{
	struct lsdc_device *lsdc = dev_get_drvdata(pdev->dev.parent);
	struct component_match *matches = NULL;
	struct loongson_drm *ldrm;

	ldrm = devm_drm_dev_alloc(pdev->dev.parent,
				  &loongson_drm_driver,
				  struct loongson_drm, ddev);
	if (IS_ERR(ldrm))
		return PTR_ERR(ldrm);

	ldrm->lsdc = lsdc;
	ldrm->gfxinfo = to_loongson_gfx(lsdc->descp);
	platform_set_drvdata(pdev, ldrm);

	loongson_matches_add(&pdev->dev, &matches);

	dev_info(&pdev->dev, "drm proxy probed\n");

	return component_master_add_with_match(&pdev->dev,
					       &loongson_drm_master_ops,
					       matches);
}

static void loongson_drm_driver_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &loongson_drm_master_ops);
}

struct platform_driver loongson_drm_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
	},
	.probe = loongson_drm_driver_probe,
	.remove_new = loongson_drm_driver_remove,
	.suspend = loongson_drm_pm_suspend,
	.resume = loongson_drm_pm_resume,
};
