// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Russell King
 *  Written from the i915 driver.
 */

#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>

#include "armada_crtc.h"
#include "armada_drm.h"
#include "armada_fb.h"
#include "armada_gem.h"

static void armada_fbdev_fb_destroy(struct fb_info *info)
{
	struct drm_fb_helper *fbh = info->par;

	drm_fb_helper_fini(fbh);

	fbh->fb->funcs->destroy(fbh->fb);

	drm_client_release(&fbh->client);
}

static const struct fb_ops armada_fb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	DRM_FB_HELPER_DEFAULT_OPS,
	.fb_destroy	= armada_fbdev_fb_destroy,
};

static const struct drm_fb_helper_funcs armada_fbdev_helper_funcs;

int armada_fbdev_driver_fbdev_probe(struct drm_fb_helper *fbh,
				    struct drm_fb_helper_surface_size *sizes)
{
	struct drm_device *dev = fbh->dev;
	struct fb_info *info = fbh->info;
	u32 fourcc, pitch;
	u64 size;
	const struct drm_format_info *format;
	struct drm_mode_fb_cmd2 mode;
	struct armada_framebuffer *dfb;
	struct armada_gem_object *obj;
	int ret;
	void *ptr;

	fourcc = drm_mode_legacy_fb_format(sizes->surface_bpp, sizes->surface_depth);
	if (fourcc == DRM_FORMAT_INVALID)
		return -EINVAL;
	format = drm_get_format_info(dev, fourcc, DRM_FORMAT_MOD_LINEAR);
	if (!format)
		return -EINVAL;
	pitch = armada_pitch(sizes->surface_width, drm_format_info_bpp(format, 0));
	if (!pitch)
		return -EINVAL;
	if (check_mul_overflow(pitch, sizes->surface_height, &size))
		return -EINVAL;
	size = ALIGN(size, PAGE_SIZE);
	if (size < PAGE_SIZE)
		return -EINVAL;

	obj = armada_gem_alloc_private_object(dev, size);
	if (!obj) {
		DRM_ERROR("failed to allocate fb memory\n");
		return -ENOMEM;
	}

	ret = armada_gem_linear_back(dev, obj);
	if (ret)
		goto err_drm_gem_object_put;

	ptr = armada_gem_map_object(dev, obj);
	if (!ptr) {
		ret = -ENOMEM;
		goto err_drm_gem_object_put;
	}

	memset(&mode, 0, sizeof(mode));
	mode.width = sizes->surface_width;
	mode.height = sizes->surface_height;
	mode.pitches[0] = pitch;
	mode.pixel_format = fourcc;

	dfb = armada_framebuffer_create(dev, format, &mode, obj);
	if (IS_ERR(dfb)) {
		ret = PTR_ERR(dfb);
		goto err_drm_gem_object_put;
	}

	info->fbops = &armada_fb_ops;
	info->fix.smem_start = obj->phys_addr;
	info->fix.smem_len = obj->obj.size;
	info->screen_size = obj->obj.size;
	info->screen_base = ptr;
	fbh->funcs = &armada_fbdev_helper_funcs;
	fbh->fb = &dfb->fb;

	drm_fb_helper_fill_info(info, fbh, sizes);

	DRM_DEBUG_KMS("allocated %dx%d %dbpp fb: 0x%08llx\n",
		dfb->fb.width, dfb->fb.height, dfb->fb.format->cpp[0] * 8,
		(unsigned long long)obj->phys_addr);

	/* The framebuffer still holds a reference on the GEM object. */
	drm_gem_object_put(&obj->obj);

	return 0;

err_drm_gem_object_put:
	drm_gem_object_put(&obj->obj);
	return ret;
}
