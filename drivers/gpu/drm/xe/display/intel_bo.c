// SPDX-License-Identifier: MIT
/* Copyright © 2024 Intel Corporation */

#include <drm/drm_cache.h>
#include <drm/drm_gem.h>

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

static int xe_panic_page = -1;
static void *xe_panic_vaddr;
static struct xe_bo *xe_panic_bo;

static void xe_panic_kunmap(void)
{
	if (xe_panic_vaddr) {
		drm_clflush_virt_range(xe_panic_vaddr, PAGE_SIZE);
		kunmap_local(xe_panic_vaddr);
		xe_panic_vaddr = NULL;
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
	unsigned int new_page;
	unsigned int offset;

	offset = y * sb->pitch[0] + x * sb->format->cpp[0];

	new_page = offset >> PAGE_SHIFT;
	offset = offset % PAGE_SIZE;
	if (new_page != xe_panic_page) {
		xe_panic_kunmap();
		xe_panic_page = new_page;
		xe_panic_vaddr = ttm_bo_kmap_try_from_panic(&xe_panic_bo->ttm,
							    xe_panic_page);
	}
	if (xe_panic_vaddr) {
		u32 *pix = xe_panic_vaddr + offset;
		*pix = color;
	}
}

int intel_bo_panic_setup(struct drm_gem_object *obj, struct drm_scanout_buffer *sb)
{
	struct xe_bo *bo = gem_to_xe_bo(obj);

	xe_panic_bo = bo;
	sb->set_pixel = xe_panic_page_set_pixel;
	return 0;
}

void intel_bo_panic_finish(struct drm_gem_object *obj)
{
	xe_panic_kunmap();
	xe_panic_page = -1;
}
