// SPDX-License-Identifier: MIT
/* Copyright © 2024 Intel Corporation */

#include <drm/drm_cache.h>
#include <drm/drm_gem.h>
#include <drm/drm_panic.h>

#include "intel_fb.h"
#include "intel_display_types.h"

#include "xe_bo.h"
#include "intel_bo.h"

bool intel_bo_is_tiled(struct drm_gem_object *obj)
{
	/* legacy tiling is unused */
	return false;
}

bool intel_bo_is_userptr(struct drm_gem_object *obj)
{
	/* xe does not have userptr bos */
	return false;
}

bool intel_bo_is_shmem(struct drm_gem_object *obj)
{
	return false;
}

bool intel_bo_is_protected(struct drm_gem_object *obj)
{
	return xe_bo_is_protected(gem_to_xe_bo(obj));
}

void intel_bo_flush_if_display(struct drm_gem_object *obj)
{
}

int intel_bo_fb_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	return drm_gem_prime_mmap(obj, vma);
}

int intel_bo_read_from_page(struct drm_gem_object *obj, u64 offset, void *dst, int size)
{
	struct xe_bo *bo = gem_to_xe_bo(obj);

	return xe_bo_read(bo, offset, dst, size);
}

struct intel_frontbuffer *intel_bo_get_frontbuffer(struct drm_gem_object *obj)
{
	return NULL;
}

struct intel_frontbuffer *intel_bo_set_frontbuffer(struct drm_gem_object *obj,
						   struct intel_frontbuffer *front)
{
	return front;
}

void intel_bo_describe(struct seq_file *m, struct drm_gem_object *obj)
{
	/* FIXME */
}

static void xe_panic_kunmap(struct intel_panic_data *panic)
{
	if (panic->vaddr) {
		drm_clflush_virt_range(panic->vaddr, PAGE_SIZE);
		kunmap_local(panic->vaddr);
		panic->vaddr = NULL;
	}
}

/*
 * The scanout buffer pages are not mapped, so for each pixel,
 * use kmap_local_page_try_from_panic() to map the page, and write the pixel.
 * Try to keep the map from the previous pixel, to avoid too much map/unmap.
 */
static void xe_panic_page_set_pixel(struct drm_scanout_buffer *sb, unsigned int x,
				    unsigned int y, u32 color)
{
	struct intel_framebuffer *fb = (struct intel_framebuffer *)sb->private;
	struct xe_bo *bo = gem_to_xe_bo(intel_fb_bo(&fb->base));
	unsigned int new_page;
	unsigned int offset;

	if (fb->panic.tiling)
		offset = fb->panic.tiling(sb->width, x, y);
	else
		offset = y * sb->pitch[0] + x * sb->format->cpp[0];

	new_page = offset >> PAGE_SHIFT;
	offset = offset % PAGE_SIZE;
	if (new_page != fb->panic.page) {
		xe_panic_kunmap(&fb->panic);
		fb->panic.page = new_page;
		fb->panic.vaddr = ttm_bo_kmap_try_from_panic(&bo->ttm,
							     fb->panic.page);
	}
	if (fb->panic.vaddr) {
		u32 *pix = fb->panic.vaddr + offset;
		*pix = color;
	}
}

int intel_bo_panic_setup(struct drm_scanout_buffer *sb)
{
	struct intel_framebuffer *fb = (struct intel_framebuffer *)sb->private;

	fb->panic.page = -1;
	sb->set_pixel = xe_panic_page_set_pixel;
	return 0;
}

void intel_bo_panic_finish(struct intel_framebuffer *fb)
{
	xe_panic_kunmap(&fb->panic);
	fb->panic.page = -1;
}
