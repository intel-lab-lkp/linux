// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include <linux/aperture.h>
#include <linux/component.h>
#include <linux/pci.h>
#include <linux/vgaarb.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "loongson_drv.h"
#include "loongson_module.h"
#include "lsdc_drv.h"

/* For multiple GPU devices co-exixt in the system */

static unsigned int lsdc_vga_set_decode(struct pci_dev *pdev, bool state)
{
	return VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM;
}

static const struct drm_mode_config_funcs lsdc_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int lsdc_modeset_init(struct lsdc_device *ldev,
			     unsigned int num_crtc,
			     const struct lsdc_kms_funcs *funcs,
			     bool has_vblank)
{
	struct drm_device *ddev = ldev->drm;
	struct lsdc_display_pipe *dispipe;
	unsigned int i;
	int ret;

	for (i = 0; i < num_crtc; i++) {
		dispipe = &ldev->dispipe[i];

		ret = funcs->primary_plane_init(ddev, &dispipe->primary.base, i);
		if (ret)
			return ret;

		ret = funcs->cursor_plane_init(ddev, &dispipe->cursor.base, i);
		if (ret)
			return ret;

		ret = funcs->crtc_init(ddev, &dispipe->crtc.base,
				       &dispipe->primary.base,
				       &dispipe->cursor.base,
				       i, has_vblank);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct drm_mode_config_helper_funcs lsdc_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail,
};

static int lsdc_mode_config_init(struct drm_device *ddev,
				 const struct lsdc_desc *descp)
{
	ddev->mode_config.funcs = &lsdc_mode_config_funcs;
	ddev->mode_config.min_width = 1;
	ddev->mode_config.min_height = 1;
	ddev->mode_config.max_width = descp->max_width * LSDC_NUM_CRTC;
	ddev->mode_config.max_height = descp->max_height * LSDC_NUM_CRTC;
	ddev->mode_config.preferred_depth = 24;
	ddev->mode_config.prefer_shadow = 1;

	ddev->mode_config.cursor_width = descp->hw_cursor_h;
	ddev->mode_config.cursor_height = descp->hw_cursor_h;

	ddev->mode_config.helper_private = &lsdc_mode_config_helper_funcs;

	if (descp->has_vblank_counter)
		ddev->max_vblank_count = 0xffffffff;

	return 0;
}

static int lsdc_pci_component_bind(struct device *dev,
				   struct device *master,
				   void *data)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct drm_device *drm = data;
	struct loongson_drm *ldrm = to_loongson_drm(drm);
	struct lsdc_device *lsdc = dev_get_drvdata(dev);
	const struct lsdc_desc *descp = lsdc->descp;
	int num_pipe = descp->num_of_crtc;
	int ret;

	ldrm->lsdc = lsdc;
	lsdc->drm = drm;

	ret = aperture_remove_conflicting_devices(ldrm->vram_base,
						  ldrm->vram_size,
						  DRIVER_NAME);
	if (ret)
		return ret;

	ret = lsdc_mode_config_init(drm, descp);
	if (ret)
		return ret;

	ret = lsdc_modeset_init(lsdc, num_pipe, descp->funcs, loongson_vblank);
	if (ret)
		return ret;

	if (loongson_vblank) {
		ret = drm_vblank_init(drm, num_pipe);
		if (ret)
			return ret;
	}

	ret = devm_request_irq(dev,
			       pdev->irq,
			       descp->funcs->irq_handler,
			       IRQF_SHARED,
			       dev_name(dev),
			       drm);
	if (ret)
		return ret;

	dev_info(dev, "lsdc irq: %d\n", pdev->irq);

	vga_client_register(pdev, lsdc_vga_set_decode);

	return 0;
}

static void lsdc_pci_component_unbind(struct device *dev,
				      struct device *master,
				      void *data)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	vga_client_unregister(pdev);
}

const struct component_ops lsdc_pci_component_ops = {
	.bind = lsdc_pci_component_bind,
	.unbind = lsdc_pci_component_unbind,
};

static int lsdc_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	const struct lsdc_desc *descp;
	struct lsdc_device *lsdc;
	int ret;

	descp = lsdc_device_probe(pdev, ent->driver_data);
	if (IS_ERR_OR_NULL(descp))
		return -ENODEV;

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "Found %s, revision: %u",
		 to_loongson_gfx(descp)->model, pdev->revision);

	lsdc = devm_kzalloc(&pdev->dev, sizeof(*lsdc), GFP_KERNEL);
	if (!lsdc)
		return -ENOMEM;

	/* Bar 0 of the DC device contains the MMIO register's base address */
	lsdc->reg_base = pcim_iomap(pdev, 0, 0);
	if (!lsdc->reg_base)
		return -ENODEV;

	lsdc->descp = descp;
	spin_lock_init(&lsdc->reglock);

	pci_set_drvdata(pdev, lsdc);

	ret = lsdc_i2c_preinit(&pdev->dev, descp);
	if (ret)
		return ret;

	ret = component_add(&pdev->dev, &lsdc_pci_component_ops);
	if (ret)
		return ret;

	ret = lsdc_output_preinit(&pdev->dev, descp);
	if (ret)
		return ret;

	ret = loongson_device_preinit(&pdev->dev);
	if (ret)
		return ret;

	return 0;
}

static void lsdc_pci_remove(struct pci_dev *pdev)
{
	component_del(&pdev->dev, &lsdc_pci_component_ops);
}

static void lsdc_pci_shutdown(struct pci_dev *pdev)
{

}

static int lsdc_pm_freeze(struct device *dev)
{
	return 0;
}

static int lsdc_pm_thaw(struct device *dev)
{
	return 0;
}

static int lsdc_pm_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	pci_save_state(pdev);
	/* Shut down the device */
	pci_disable_device(pdev);
	pci_set_power_state(pdev, PCI_D3hot);

	return 0;
}

static int lsdc_pm_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	pci_set_power_state(pdev, PCI_D0);

	pci_restore_state(pdev);

	if (pcim_enable_device(pdev))
		return -EIO;

	return 0;
}

static const struct dev_pm_ops lsdc_pm_ops = {
	.suspend = lsdc_pm_suspend,
	.resume = lsdc_pm_resume,
	.freeze = lsdc_pm_freeze,
	.thaw = lsdc_pm_thaw,
};

static const struct pci_device_id lsdc_pciid_list[] = {
	{PCI_VDEVICE(LOONGSON, 0x7a06), CHIP_LS7A1000},
	{PCI_VDEVICE(LOONGSON, 0x7a36), CHIP_LS7A2000},
	{ }
};

struct pci_driver lsdc_pci_driver = {
	.name = "loongson.lsdc",
	.id_table = lsdc_pciid_list,
	.probe = lsdc_pci_probe,
	.remove = lsdc_pci_remove,
	.shutdown = lsdc_pci_shutdown,
	.driver.pm = &lsdc_pm_ops,
};

MODULE_DEVICE_TABLE(pci, lsdc_pciid_list);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
