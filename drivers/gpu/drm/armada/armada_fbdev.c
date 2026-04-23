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

	drm_client_buffer_delete(fbh->buffer);
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
	struct drm_client_dev *client = &fbh->client;
	struct drm_device *dev = client->dev;
	struct drm_file *file = client->file;
	struct fb_info *info = fbh->info;
	u32 fourcc, pitch;
	u64 size;
	const struct drm_format_info *format;
	struct armada_gem_object *obj;
	struct drm_client_buffer *buffer;
	u32 handle;
	int ret;
	void *ptr;

	fourcc = drm_mode_legacy_fb_format(sizes->surface_bpp, sizes->surface_depth);
	format = drm_get_format_info(dev, fourcc, DRM_FORMAT_MOD_LINEAR);
	pitch = armada_pitch(sizes->surface_width, drm_format_info_bpp(format, 0));
	size = ALIGN(pitch * sizes->surface_height, PAGE_SIZE);

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

	ret = drm_gem_handle_create(file, &obj->obj, &handle);
	if (ret)
		goto err_drm_gem_object_put;

	buffer = drm_client_buffer_create(client, sizes->surface_width, sizes->surface_height,
					  fourcc, handle, pitch);
	if (IS_ERR(buffer)) {
		ret = PTR_ERR(buffer);
		goto err_drm_gem_handle_delete;
	}

	fbh->funcs = &armada_fbdev_helper_funcs;
	fbh->buffer = buffer;
	fbh->fb = buffer->fb;

	info->fbops = &armada_fb_ops;
	info->fix.smem_start = obj->phys_addr;
	info->fix.smem_len = obj->obj.size;
	info->screen_size = obj->obj.size;
	info->screen_base = ptr;

	drm_fb_helper_fill_info(info, fbh, sizes);

	DRM_DEBUG_KMS("allocated %dx%d %dbpp fb: 0x%08llx\n",
		buffer->fb->width, buffer->fb->height, buffer->fb->format->cpp[0] * 8,
		(unsigned long long)obj->phys_addr);

	/* The handle is only needed for creating the framebuffer. */
	drm_gem_handle_delete(file, handle);

	/* The framebuffer still holds a reference on the GEM object. */
	drm_gem_object_put(&obj->obj);

	return 0;

err_drm_gem_handle_delete:
	drm_gem_handle_delete(file, handle);
err_drm_gem_object_put:
	drm_gem_object_put(&obj->obj);
	return ret;
}
