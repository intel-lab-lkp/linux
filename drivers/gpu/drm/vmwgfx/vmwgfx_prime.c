// SPDX-License-Identifier: GPL-2.0 OR MIT
/**************************************************************************
 *
 * Copyright (c) 2013-2024 Broadcom. All Rights Reserved. The term
 * “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors:
 *     Thomas Hellstrom <thellstrom@vmware.com>
 *
 */

#include "vmwgfx_drv.h"
#include "vmwgfx_bo.h"
#include "ttm_object.h"
#include <linux/dma-buf.h>
#include <linux/slab.h>

/*
 * DMA-BUF attach- and mapping methods. No need to implement
 * these until we have other virtual devices use them.
 */

static int vmw_prime_map_attach(struct dma_buf *dma_buf,
				struct dma_buf_attachment *attach)
{
	return -ENOSYS;
}

static void vmw_prime_map_detach(struct dma_buf *dma_buf,
				 struct dma_buf_attachment *attach)
{
}

static struct sg_table *vmw_prime_map_dma_buf(struct dma_buf_attachment *attach,
					      enum dma_data_direction dir)
{
	return ERR_PTR(-ENOSYS);
}

static void vmw_prime_unmap_dma_buf(struct dma_buf_attachment *attach,
				    struct sg_table *sgb,
				    enum dma_data_direction dir)
{
}

const struct dma_buf_ops vmw_prime_dmabuf_ops =  {
	.attach = vmw_prime_map_attach,
	.detach = vmw_prime_map_detach,
	.map_dma_buf = vmw_prime_map_dma_buf,
	.unmap_dma_buf = vmw_prime_unmap_dma_buf,
	.release = NULL,
};

/*
 * A surface-backed dma-buf can only be imported through
 * ttm_prime_fd_to_handle() -- vmw_prime_dmabuf_ops leaves .attach and
 * .map_dma_buf unimplemented above, so the generic PRIME import path
 * (drm_gem_prime_fd_to_handle() -> dma_buf_attach()) can never reach it and
 * fails with -ENOSYS before it starts. ttm_prime_fd_to_handle() works
 * because it bypasses dma-buf attach/map entirely and reads dma_buf->priv
 * directly, but the handle it returns lives in the private ttm_object
 * table (tdev->idr), not file_priv->object_idr, so the generic
 * DRM_IOCTL_GEM_CLOSE (drm_gem_handle_delete(), which only ever looks in
 * file_priv->object_idr) can never find it and fails with -EINVAL.
 *
 * Bridge the two tables: wrap the real ttm_base_object in a minimal,
 * non-TTM-backed GEM object and hand back a handle from the standard
 * table instead, so ordinary GEM_CLOSE succeeds.
 *
 * Existing userspace (Mesa's own SVGA winsys, in vmw_drm_surface_from_handle())
 * calls this exact ioctl for real rendering imports too, then feeds the
 * returned value straight back into DRM_VMW_REF_SURFACE / DRM_VMW_UNREF_SURFACE
 * as a raw ttm handle -- it is not just a probe-only code path. So the value
 * returned here must keep working as a raw ttm handle for those two ioctls
 * and for every execbuf command that references a surface by handle
 * (vmw_user_resource_lookup_handle(), the single choke point all of those
 * funnel through). vmw_prime_resolve_handle() below is called from all three
 * of those places to transparently redirect a bridge handle back to the real
 * ttm handle, so existing userspace keeps working unmodified.
 *
 * The bridge's own hold on the object is a *separate*, independent
 * ttm_base_object reference (taken via ttm_base_object_lookup_for_ref(),
 * released via ttm_base_object_unref()), not a claim on the tfile-scoped
 * ttm_ref_object entry that REF_SURFACE/UNREF_SURFACE manipulate. That
 * entry is created transiently by ttm_prime_fd_to_handle() below and is
 * deliberately left alone on success -- see the comment at the end of
 * vmw_prime_fd_to_handle() for why it cannot simply be dropped here
 * (vmw_surface_handle_reference()'s require_exist path depends on it
 * still existing). This bridge's own reference exists alongside it,
 * purely so this bridge's lifetime never depends on how many times
 * userspace itself opens and closes references to the same ttm handle:
 * without it, if userspace fully released the ttm handle through its own
 * REF/UNREF_SURFACE calls while this GEM handle was still open, this
 * bridge's eventual .free() would unref a handle number that may by then
 * have been recycled for a completely unrelated object.
 */
struct vmw_prime_import_bridge {
	struct drm_gem_object    base;
	struct ttm_base_object  *base_obj;
	uint32_t                 ttm_handle;
};

static void vmw_prime_import_bridge_free(struct drm_gem_object *obj)
{
	struct vmw_prime_import_bridge *bridge =
		container_of(obj, struct vmw_prime_import_bridge, base);

	ttm_base_object_unref(&bridge->base_obj);
	drm_gem_object_release(obj);
	kfree(bridge);
}

static const struct drm_gem_object_funcs vmw_prime_import_bridge_funcs = {
	.free = vmw_prime_import_bridge_free,
};

/**
 * vmw_prime_resolve_handle - Translate a possible prime-import bridge GEM
 * handle back to the real ttm handle it wraps.
 *
 * @file_priv: The caller's drm file, whose own GEM handle table is checked.
 * @handle: A handle as supplied by userspace -- either an ordinary raw ttm
 * handle (the common case, unchanged from historical behavior), or a
 * bridge handle returned by vmw_prime_fd_to_handle() above.
 *
 * Returns the real ttm handle to use. If @handle does not name one of this
 * file's own prime-import bridge objects, @handle is returned unchanged.
 */
uint32_t vmw_prime_resolve_handle(struct drm_file *file_priv, uint32_t handle)
{
	struct drm_gem_object *gobj = drm_gem_object_lookup(file_priv, handle);
	uint32_t real_handle = handle;

	if (gobj) {
		if (gobj->funcs == &vmw_prime_import_bridge_funcs) {
			struct vmw_prime_import_bridge *bridge =
				container_of(gobj, struct vmw_prime_import_bridge, base);
			real_handle = bridge->ttm_handle;
		}
		drm_gem_object_put(gobj);
	}

	return real_handle;
}

int vmw_prime_fd_to_handle(struct drm_device *dev,
			   struct drm_file *file_priv,
			   int fd, u32 *handle)
{
	struct vmw_private *dev_priv = vmw_priv(dev);
	struct ttm_object_file *tfile = vmw_fpriv(file_priv)->tfile;
	struct vmw_prime_import_bridge *bridge;
	struct ttm_base_object *base_obj;
	uint32_t ttm_handle;
	int ret = ttm_prime_fd_to_handle(tfile, fd, &ttm_handle);

	if (ret)
		return drm_gem_prime_fd_to_handle(dev, file_priv, fd, handle);

	/*
	 * Take our own independent reference before dropping the transient
	 * one ttm_prime_fd_to_handle() just created, so the object can never
	 * be dropped to zero in between.
	 */
	base_obj = ttm_base_object_lookup_for_ref(dev_priv->tdev, ttm_handle);
	if (!base_obj) {
		ttm_ref_object_base_unref(tfile, ttm_handle);
		return -EINVAL;
	}

	bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
	if (!bridge) {
		ttm_base_object_unref(&base_obj);
		ttm_ref_object_base_unref(tfile, ttm_handle);
		return -ENOMEM;
	}

	drm_gem_private_object_init(dev, &bridge->base, PAGE_SIZE);
	bridge->base.funcs = &vmw_prime_import_bridge_funcs;
	bridge->base_obj   = base_obj;
	bridge->ttm_handle = ttm_handle;

	ret = drm_gem_handle_create(file_priv, &bridge->base, handle);
	drm_gem_object_put(&bridge->base);

	/*
	 * On success, deliberately leave the transient ttm_ref_object entry
	 * ttm_prime_fd_to_handle() created in place -- do not touch it.
	 * vmw_surface_handle_reference()'s DRM_VMW_REF_SURFACE path forces
	 * require_exist=true for render clients (drm_is_render_client()),
	 * which is exactly what Mesa and this bridge's own callers are, and
	 * ttm_ref_object_add() with require_existed=true returns -EPERM
	 * unless a ref-object entry for this (tfile, ttm_handle) pair
	 * already exists -- it will not create a new one. This entry is
	 * that pre-existing one. Stock vmw_prime_fd_to_handle() never
	 * touched it either, for the same reason. Our own independent
	 * base_obj reference above is what makes GEM_CLOSE work; it does
	 * not replace this entry, it exists alongside it.
	 *
	 * On failure, nothing else will ever be able to reach this handle to
	 * release it, so clean it up here to avoid leaking it.
	 */
	if (ret)
		ttm_ref_object_base_unref(tfile, ttm_handle);

	return ret;
}

int vmw_prime_handle_to_fd(struct drm_device *dev,
			   struct drm_file *file_priv,
			   uint32_t handle, uint32_t flags,
			   int *prime_fd)
{
	struct vmw_private *vmw = vmw_priv(dev);
	struct ttm_object_file *tfile = vmw_fpriv(file_priv)->tfile;
	struct vmw_bo *vbo;
	int ret;
	int surf_handle;

	if (handle > VMWGFX_NUM_MOB) {
		ret = ttm_prime_handle_to_fd(tfile, handle, flags, prime_fd);
	} else {
		ret = vmw_user_bo_lookup(file_priv, handle, &vbo);
		if (ret)
			return ret;
		if (vbo && vbo->is_dumb) {
			ret = drm_gem_prime_handle_to_fd(dev, file_priv, handle,
							 flags, prime_fd);
		} else {
			surf_handle = vmw_lookup_surface_handle_for_buffer(vmw,
									   vbo,
									   handle);
			if (surf_handle > 0)
				ret = ttm_prime_handle_to_fd(tfile, surf_handle,
							     flags, prime_fd);
			else
				ret = drm_gem_prime_handle_to_fd(dev, file_priv,
								 handle, flags,
								 prime_fd);
		}
		vmw_user_bo_unref(&vbo);
	}

	return ret;
}
