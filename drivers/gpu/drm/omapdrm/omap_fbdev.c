// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011 Texas Instruments Incorporated - https://www.ti.com/
 * Author: Rob Clark <rob@ti.com>
 */

#include <linux/fb.h>
#include <linux/overflow.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_drv.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_util.h>

#include "omap_drv.h"
#include "omap_fbdev.h"

MODULE_PARM_DESC(ywrap, "Enable ywrap scrolling (omap44xx and later, default 'y')");
static bool ywrap_enabled = true;
module_param_named(ywrap, ywrap_enabled, bool, 0644);

/*
 * fbdev funcs, to implement legacy fbdev interface on top of drm driver
 */

struct omap_fbdev {
	struct drm_device *dev;
	bool ywrap_enabled;

	/* for deferred dmm roll when getting called in atomic ctx */
	struct work_struct work;
};

static struct drm_fb_helper *get_fb(struct fb_info *fbi);

static void pan_worker(struct work_struct *work)
{
	struct omap_fbdev *fbdev = container_of(work, struct omap_fbdev, work);
	struct drm_fb_helper *helper = fbdev->dev->fb_helper;
	struct fb_info *fbi = helper->info;
	struct drm_gem_object *bo = drm_gem_fb_get_obj(helper->fb, 0);
	int npages;

	/* DMM roll shifts in 4K pages: */
	npages = fbi->fix.line_length >> PAGE_SHIFT;
	omap_gem_roll(bo, fbi->var.yoffset * npages);
}

FB_GEN_DEFAULT_DEFERRED_DMAMEM_OPS(omap_fbdev,
				   drm_fb_helper_damage_range,
				   drm_fb_helper_damage_area)

static int omap_fbdev_pan_display(struct fb_var_screeninfo *var, struct fb_info *fbi)
{
	struct drm_fb_helper *helper = get_fb(fbi);
	struct omap_drm_private *priv;
	struct omap_fbdev *fbdev;

	if (!helper)
		goto fallback;

	priv = helper->dev->dev_private;
	fbdev = priv->fbdev;

	if (!fbdev->ywrap_enabled)
		goto fallback;

	if (drm_can_sleep())
		pan_worker(&fbdev->work);
	else
		queue_work(priv->wq, &fbdev->work);

	return 0;

fallback:
	return drm_fb_helper_pan_display(var, fbi);
}

static int omap_fbdev_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));

	return fb_deferred_io_mmap(info, vma);
}

static void omap_fbdev_fb_destroy(struct fb_info *info)
{
	struct drm_fb_helper *helper = info->par;
	struct drm_gem_object *bo = drm_gem_fb_get_obj(helper->fb, 0);

	DBG();

	fb_deferred_io_cleanup(info);
	drm_fb_helper_fini(helper);

	omap_gem_unpin(bo);

	drm_client_buffer_delete(helper->buffer);
	drm_client_release(&helper->client);
}

/*
 * For now, we cannot use FB_DEFAULT_DEFERRED_OPS and fb_deferred_io_mmap()
 * because we use write-combine.
 */
static const struct fb_ops omap_fb_ops = {
	.owner = THIS_MODULE,
	__FB_DEFAULT_DEFERRED_OPS_RDWR(omap_fbdev),
	.fb_check_var	= drm_fb_helper_check_var,
	.fb_set_par	= drm_fb_helper_set_par,
	.fb_setcmap	= drm_fb_helper_setcmap,
	.fb_blank	= drm_fb_helper_blank,
	.fb_pan_display = omap_fbdev_pan_display,
	__FB_DEFAULT_DEFERRED_OPS_DRAW(omap_fbdev),
	.fb_ioctl	= drm_fb_helper_ioctl,
	.fb_mmap	= omap_fbdev_fb_mmap,
	.fb_destroy	= omap_fbdev_fb_destroy,
};

static int omap_fbdev_dirty(struct drm_fb_helper *helper, struct drm_clip_rect *clip)
{
	if (!(clip->x1 < clip->x2 && clip->y1 < clip->y2))
		return 0;

	if (helper->fb->funcs->dirty)
		return helper->fb->funcs->dirty(helper->fb, NULL, 0, 0, clip, 1);

	return 0;
}

static const struct drm_fb_helper_funcs omap_fbdev_helper_funcs = {
	.fb_dirty = omap_fbdev_dirty,
};

static struct drm_fb_helper *get_fb(struct fb_info *fbi)
{
	if (!fbi || strcmp(fbi->fix.id, MODULE_NAME)) {
		/* these are not the fb's you're looking for */
		return NULL;
	}
	return fbi->par;
}

int omap_fbdev_driver_fbdev_probe(struct drm_fb_helper *helper,
				  struct drm_fb_helper_surface_size *sizes)
{
	struct drm_client_dev *client = &helper->client;
	struct drm_device *dev = client->dev;
	struct drm_file *file = client->file;
	struct omap_drm_private *priv = dev->dev_private;
	struct omap_fbdev *fbdev = priv->fbdev;
	struct fb_info *fbi = helper->info;
	u32 fourcc;
	u64 pitch, size;
	const struct drm_format_info *format;
	union omap_gem_size gsize;
	struct drm_gem_object *bo;
	struct drm_client_buffer *buffer;
	dma_addr_t dma_addr;
	u32 handle;
	int ret;

	sizes->surface_bpp = 32;
	sizes->surface_depth = 24;

	DBG("create fbdev: %dx%d@%d (%dx%d)", sizes->surface_width,
			sizes->surface_height, sizes->surface_bpp,
			sizes->fb_width, sizes->fb_height);

	fbdev->ywrap_enabled = priv->has_dmm && ywrap_enabled;

	fourcc = drm_mode_legacy_fb_format(sizes->surface_bpp, sizes->surface_depth);
	format = drm_get_format_info(dev, fourcc, DRM_FORMAT_MOD_LINEAR);
	if (!format)
		return -EINVAL;
	pitch = drm_format_info_min_pitch(format, 0, sizes->surface_width);
	if (!pitch)
		return -EINVAL;
	if (fbdev->ywrap_enabled)
		pitch = PAGE_ALIGN(pitch); /* required if using DMM scrolling */
	if (check_mul_overflow(pitch, sizes->surface_height, &size))
		return -EINVAL;
	size = PAGE_ALIGN(size);

	/* allocate backing bo */
	gsize = (union omap_gem_size){
		.bytes = size,
	};
	DBG("allocating %d bytes for fb %d", gsize.bytes, dev->primary->index);
	bo = omap_gem_new(dev, gsize, OMAP_BO_SCANOUT | OMAP_BO_WC);
	if (!bo) {
		dev_err(dev->dev, "failed to allocate buffer object\n");
		return -ENOMEM;
	}

	ret = drm_gem_handle_create(file, bo, &handle);
	if (ret)
		goto err_drm_gem_object_put;

	buffer = drm_client_buffer_create(client, sizes->surface_width, sizes->surface_height,
					  fourcc, handle, pitch);
	if (IS_ERR(buffer)) {
		ret = PTR_ERR(buffer);
		goto err_drm_gem_handle_delete;
	}

	/* note: this keeps the bo pinned.. which is perhaps not ideal,
	 * but is needed as long as we use fb_mmap() to mmap to userspace
	 * (since this happens using fix.smem_start).  Possibly we could
	 * implement our own mmap using GEM mmap support to avoid this
	 * (non-tiled buffer doesn't need to be pinned for fbcon to write
	 * to it).  Then we just need to be sure that we are able to re-
	 * pin it in case of an opps.
	 */
	ret = omap_gem_pin(bo, &dma_addr);
	if (ret) {
		dev_err(dev->dev, "could not pin framebuffer\n");
		ret = -ENOMEM;
		goto err_drm_client_buffer_delete;
	}

	DBG("fbi=%p, dev=%p", fbi, dev);

	helper->funcs = &omap_fbdev_helper_funcs;
	helper->buffer = buffer;
	helper->fb = buffer->fb;

	fbi->fbops = &omap_fb_ops;

	drm_fb_helper_fill_info(fbi, helper, sizes);

	fbi->flags |= FBINFO_VIRTFB;
	fbi->screen_buffer = omap_gem_vaddr(bo);
	fbi->screen_size = bo->size;
	fbi->fix.smem_start = dma_addr;
	fbi->fix.smem_len = bo->size;

	/* deferred I/O */
	helper->fbdefio.delay = HZ / 20;
	helper->fbdefio.deferred_io = drm_fb_helper_deferred_io;

	fbi->fbdefio = &helper->fbdefio;
	ret = fb_deferred_io_init(fbi);
	if (ret)
		goto err_omap_gem_unpin;

	/* if we have DMM, then we can use it for scrolling by just
	 * shuffling pages around in DMM rather than doing sw blit.
	 */
	if (fbdev->ywrap_enabled) {
		DRM_INFO("Enabling DMM ywrap scrolling\n");
		fbi->flags |= FBINFO_HWACCEL_YWRAP | FBINFO_READS_FAST;
		fbi->fix.ywrapstep = 1;
	}

	DBG("par=%p, %dx%d", fbi->par, fbi->var.xres, fbi->var.yres);
	DBG("allocated %dx%d fb", buffer->fb->width, buffer->fb->height);

	/* The handle is only needed for creating the framebuffer. */
	drm_gem_handle_delete(file, handle);

	/* The framebuffer still holds a reference on the GEM object. */
	drm_gem_object_put(bo);

	return 0;

err_omap_gem_unpin:
	omap_gem_unpin(bo);
err_drm_client_buffer_delete:
	drm_client_buffer_delete(buffer);
err_drm_gem_handle_delete:
	drm_gem_handle_delete(file, handle);
err_drm_gem_object_put:
	drm_gem_object_put(bo);
	return ret;
}

void omap_fbdev_setup(struct drm_device *dev)
{
	struct omap_drm_private *priv = dev->dev_private;
	struct omap_fbdev *fbdev;

	drm_WARN(dev, !dev->registered, "Device has not been registered.\n");
	drm_WARN(dev, dev->fb_helper, "fb_helper is already set!\n");

	fbdev = drmm_kzalloc(dev, sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev)
		return;
	fbdev->dev = dev;
	INIT_WORK(&fbdev->work, pan_worker);

	priv->fbdev = fbdev;

	drm_client_setup(dev, NULL);
}
