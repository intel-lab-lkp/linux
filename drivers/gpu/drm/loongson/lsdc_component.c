// SPDX-License-Identifier: GPL-2.0+

/*
 * Authors:
 *      Sui Jingfeng <sui.jingfeng@linux.dev>
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

#include "loongson_module.h"
#include "loongson_drv.h"
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
