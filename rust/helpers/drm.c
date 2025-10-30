// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_buddy.h>
#include <drm/drm_gem.h>
#include <drm/drm_vma_manager.h>

#ifdef CONFIG_DRM

void rust_helper_drm_gem_object_get(struct drm_gem_object *obj)
{
	drm_gem_object_get(obj);
}

void rust_helper_drm_gem_object_put(struct drm_gem_object *obj)
{
	drm_gem_object_put(obj);
}

__u64 rust_helper_drm_vma_node_offset_addr(struct drm_vma_offset_node *node)
{
	return drm_vma_node_offset_addr(node);
}

#ifdef CONFIG_DRM_BUDDY

u64 rust_helper_drm_buddy_block_offset(const struct drm_buddy_block *block)
{
	return drm_buddy_block_offset(block);
}

unsigned int rust_helper_drm_buddy_block_order(struct drm_buddy_block *block)
{
	return drm_buddy_block_order(block);
}

u64 rust_helper_drm_buddy_block_size(struct drm_buddy *mm,
				     struct drm_buddy_block *block)
{
	return drm_buddy_block_size(mm, block);
}

#endif /* CONFIG_DRM_BUDDY */

#endif /* CONFIG_DRM */
