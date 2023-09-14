/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __DRM_GEM_SHMEM_HELPER_H__
#define __DRM_GEM_SHMEM_HELPER_H__

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/shrinker.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_prime.h>

struct dma_buf_attachment;
struct drm_device;
struct drm_mode_create_dumb;
struct drm_printer;
struct sg_table;

/**
 * struct drm_gem_shmem_object - GEM object backed by shmem
 */
struct drm_gem_shmem_object {
	/**
	 * @base: Base GEM object
	 */
	struct drm_gem_object base;

	/**
	 * @pages: Page table
	 */
	struct page **pages;

	/**
	 * @pages_use_count:
	 *
	 * Reference count on the pages table.
	 * The pages are put when the count reaches zero.
	 */
	refcount_t pages_use_count;

	/**
	 * @pages_pin_count:
	 *
	 * Reference count on the pinned pages table.
	 * The pages allowed to be evicted and purged by memory
	 * shrinker only when the count is zero, otherwise pages
	 * are hard-pinned in memory.
	 */
	refcount_t pages_pin_count;

	/**
	 * @madv: State for madvise
	 *
	 * 0 is active/inuse.
	 * 1 is not-needed/can-be-purged
	 * A negative value is the object is purged.
	 */
	int madv;

	/**
	 * @madv_list: List entry for madvise tracking
	 *
	 * Typically used by drivers to track purgeable objects
	 */
	struct list_head madv_list;

	/**
	 * @sgt: Scatter/gather table for imported PRIME buffers
	 */
	struct sg_table *sgt;

	/**
	 * @vaddr: Kernel virtual address of the backing memory
	 */
	void *vaddr;

	/**
	 * @vmap_use_count:
	 *
	 * Reference count on the virtual address.
	 * The address are un-mapped when the count reaches zero.
	 */
	refcount_t vmap_use_count;

	/**
	 * @pages_mark_dirty_on_put:
	 *
	 * Mark pages as dirty when they are put.
	 */
	bool pages_mark_dirty_on_put : 1;

	/**
	 * @pages_mark_accessed_on_put:
	 *
	 * Mark pages as accessed when they are put.
	 */
	bool pages_mark_accessed_on_put : 1;

	/**
	 * @map_wc: map object write-combined (instead of using shmem defaults).
	 */
	bool map_wc : 1;

	/**
	 * @evicted: True if shmem pages are evicted by the memory shrinker.
	 * Used internally by memory shrinker.
	 */
	bool evicted : 1;
};

#define to_drm_gem_shmem_obj(obj) \
	container_of(obj, struct drm_gem_shmem_object, base)

struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *dev, size_t size);
void drm_gem_shmem_free(struct drm_gem_shmem_object *shmem);

void drm_gem_shmem_put_pages_locked(struct drm_gem_shmem_object *shmem);
int drm_gem_shmem_pin(struct drm_gem_shmem_object *shmem);
void drm_gem_shmem_unpin(struct drm_gem_shmem_object *shmem);
int drm_gem_shmem_vmap_locked(struct drm_gem_shmem_object *shmem,
			      struct iosys_map *map);
void drm_gem_shmem_vunmap_locked(struct drm_gem_shmem_object *shmem,
				 struct iosys_map *map);
int drm_gem_shmem_mmap(struct drm_gem_shmem_object *shmem, struct vm_area_struct *vma);

int drm_gem_shmem_madvise_locked(struct drm_gem_shmem_object *shmem, int madv);
int drm_gem_shmem_madvise(struct drm_gem_shmem_object *shmem, int madv);

static inline bool drm_gem_shmem_is_purgeable(struct drm_gem_shmem_object *shmem)
{
	dma_resv_assert_held(shmem->base.resv);

	return (shmem->madv > 0) && shmem->base.funcs->evict &&
		refcount_read(&shmem->pages_use_count) &&
		!refcount_read(&shmem->pages_pin_count) &&
		!shmem->base.dma_buf && !shmem->base.import_attach &&
		(shmem->sgt || shmem->evicted);
}

int drm_gem_shmem_swapin_locked(struct drm_gem_shmem_object *shmem);

void drm_gem_shmem_evict_locked(struct drm_gem_shmem_object *shmem);
void drm_gem_shmem_purge_locked(struct drm_gem_shmem_object *shmem);

struct sg_table *drm_gem_shmem_get_sg_table(struct drm_gem_shmem_object *shmem);
struct sg_table *drm_gem_shmem_get_pages_sgt(struct drm_gem_shmem_object *shmem);

void drm_gem_shmem_print_info(const struct drm_gem_shmem_object *shmem,
			      struct drm_printer *p, unsigned int indent);

extern const struct vm_operations_struct drm_gem_shmem_vm_ops;

/*
 * GEM object functions
 */

/**
 * drm_gem_shmem_object_free - GEM object function for drm_gem_shmem_free()
 * @obj: GEM object to free
 *
 * This function wraps drm_gem_shmem_free(). Drivers that employ the shmem helpers
 * should use it as their &drm_gem_object_funcs.free handler.
 */
static inline void drm_gem_shmem_object_free(struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_gem_shmem_free(shmem);
}

/**
 * drm_gem_shmem_object_print_info() - Print &drm_gem_shmem_object info for debugfs
 * @p: DRM printer
 * @indent: Tab indentation level
 * @obj: GEM object
 *
 * This function wraps drm_gem_shmem_print_info(). Drivers that employ the shmem helpers should
 * use this function as their &drm_gem_object_funcs.print_info handler.
 */
static inline void drm_gem_shmem_object_print_info(struct drm_printer *p, unsigned int indent,
						   const struct drm_gem_object *obj)
{
	const struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_gem_shmem_print_info(shmem, p, indent);
}

/**
 * drm_gem_shmem_object_pin - GEM object function for drm_gem_shmem_pin()
 * @obj: GEM object
 *
 * This function wraps drm_gem_shmem_pin(). Drivers that employ the shmem helpers should
 * use it as their &drm_gem_object_funcs.pin handler.
 */
static inline int drm_gem_shmem_object_pin(struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	return drm_gem_shmem_pin(shmem);
}

/**
 * drm_gem_shmem_object_unpin - GEM object function for drm_gem_shmem_unpin()
 * @obj: GEM object
 *
 * This function wraps drm_gem_shmem_unpin(). Drivers that employ the shmem helpers should
 * use it as their &drm_gem_object_funcs.unpin handler.
 */
static inline void drm_gem_shmem_object_unpin(struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_gem_shmem_unpin(shmem);
}

/**
 * drm_gem_shmem_object_get_sg_table - GEM object function for drm_gem_shmem_get_sg_table()
 * @obj: GEM object
 *
 * This function wraps drm_gem_shmem_get_sg_table(). Drivers that employ the shmem helpers should
 * use it as their &drm_gem_object_funcs.get_sg_table handler.
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or error pointer on failure.
 */
static inline struct sg_table *drm_gem_shmem_object_get_sg_table(struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	return drm_gem_shmem_get_sg_table(shmem);
}

/*
 * drm_gem_shmem_object_vmap_locked - GEM object function for drm_gem_shmem_vmap_locked()
 * @obj: GEM object
 * @map: Returns the kernel virtual address of the SHMEM GEM object's backing store.
 *
 * This function wraps drm_gem_shmem_vmap_locked(). Drivers that employ the shmem
 * helpers should use it as their &drm_gem_object_funcs.vmap handler.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
static inline int drm_gem_shmem_object_vmap_locked(struct drm_gem_object *obj,
						   struct iosys_map *map)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	return drm_gem_shmem_vmap_locked(shmem, map);
}

/*
 * drm_gem_shmem_object_vunmap - GEM object function for drm_gem_shmem_vunmap()
 * @obj: GEM object
 * @map: Kernel virtual address where the SHMEM GEM object was mapped
 *
 * This function wraps drm_gem_shmem_vunmap_locked(). Drivers that employ the shmem
 * helpers should use it as their &drm_gem_object_funcs.vunmap handler.
 */
static inline void drm_gem_shmem_object_vunmap_locked(struct drm_gem_object *obj,
						      struct iosys_map *map)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_gem_shmem_vunmap_locked(shmem, map);
}

/**
 * drm_gem_shmem_object_mmap - GEM object function for drm_gem_shmem_mmap()
 * @obj: GEM object
 * @vma: VMA for the area to be mapped
 *
 * This function wraps drm_gem_shmem_mmap(). Drivers that employ the shmem helpers should
 * use it as their &drm_gem_object_funcs.mmap handler.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
static inline int drm_gem_shmem_object_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	return drm_gem_shmem_mmap(shmem, vma);
}

/**
 * drm_gem_shmem_object_madvise - unlocked GEM object function for drm_gem_shmem_madvise_locked()
 * @obj: GEM object
 * @madv: Madvise value
 *
 * This function wraps drm_gem_shmem_madvise_locked(), providing unlocked variant.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
static inline int drm_gem_shmem_object_madvise(struct drm_gem_object *obj, int madv)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	return drm_gem_shmem_madvise(shmem, madv);
}

/**
 * struct drm_gem_shmem_shrinker - Memory shrinker of GEM shmem memory manager
 */
struct drm_gem_shmem_shrinker {
	/** @base: Shrinker for purging shmem GEM objects */
	struct shrinker base;

	/** @lock: Protects @lru_* */
	struct mutex lock;

	/** @lru_pinned: List of pinned shmem GEM objects */
	struct drm_gem_lru lru_pinned;

	/** @lru_evictable: List of shmem GEM objects to be evicted */
	struct drm_gem_lru lru_evictable;

	/** @lru_evicted: List of evicted shmem GEM objects */
	struct drm_gem_lru lru_evicted;
};

/**
 * struct drm_gem_shmem - GEM shmem memory manager
 */
struct drm_gem_shmem {
	/** @shrinker: GEM shmem shrinker */
	struct drm_gem_shmem_shrinker shrinker;
};

int drmm_gem_shmem_init(struct drm_device *dev);

/*
 * Driver ops
 */

struct drm_gem_object *
drm_gem_shmem_prime_import_sg_table(struct drm_device *dev,
				    struct dma_buf_attachment *attach,
				    struct sg_table *sgt);
int drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
			      struct drm_mode_create_dumb *args);

/**
 * DRM_GEM_SHMEM_DRIVER_OPS - Default shmem GEM operations
 *
 * This macro provides a shortcut for setting the shmem GEM operations in
 * the &drm_driver structure.
 */
#define DRM_GEM_SHMEM_DRIVER_OPS \
	.gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table, \
	.dumb_create		   = drm_gem_shmem_dumb_create

#endif /* __DRM_GEM_SHMEM_HELPER_H__ */
