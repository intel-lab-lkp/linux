/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef __DRM_RAS_H__
#define __DRM_RAS_H__

#include <linux/types.h>

#include <uapi/drm/drm_ras.h>

/**
 * struct drm_ras_retired_resource - A single retired resource entry
 *
 * Describes one hardware resource that has been taken out of service and is
 * reported by a %DRM_RAS_NODE_TYPE_RETIRED_RESOURCES node. @type selects which
 * member of the anonymous union is valid, allowing new hardware resource types
 * to be added without changing the common fields. When @is_summary is set the
 * entry is a leading, type-specific capacity summary (e.g. max retirable
 * pages) rather than an individual retired resource.
 */
struct drm_ras_retired_resource {
	/** @type: Resource type (enum drm_ras_retired_resource_type). */
	__u32 type;
	/** @status: Retirement status (enum drm_ras_retired_resource_status). */
	__u32 status;
	/**
	 * @key: Opaque, driver-assigned ordering key for this entry. drm-ras
	 * uses it only to advance the dump cursor; entries must be enumerable
	 * in strictly increasing @key order and keys must be unique.
	 */
	__u64 key;
	/** @is_summary: entry reports type capacity, not a retired resource. */
	__u8 is_summary;
	union {
		/** @vram_page: Valid when @type is VRAM_PAGE. */
		struct {
			/** @vram_page.address: Device address (e.g. DPA). */
			__u64 address;
			/** @vram_page.size: Size in bytes. */
			__u64 size;
			/** @vram_page.max_pages: max retirable pages; summary only. */
			__u32 max_pages;
		} vram_page;
	};
};

/**
 * struct drm_ras_node - A DRM RAS Node
 */
struct drm_ras_node {
	/** @id: Unique identifier for the node. Dynamically assigned. */
	u32 id;
	/**
	 * @device_name: Human-readable name of the device. Given by the driver.
	 */
	const char *device_name;
	/** @node_name: Human-readable name of the node. Given by the driver. */
	const char *node_name;
	/** @type: Type of the node (enum drm_ras_node_type). */
	enum drm_ras_node_type type;

	/* Error-Counter Related Callback and Variables */

	/** @error_counter_range: Range of valid Error IDs for this node. */
	struct {
		/** @first: First valid Error ID. */
		u32 first;
		/** @last: Last valid Error ID. Mandatory entry. */
		u32 last;
	} error_counter_range;

	/**
	 * @query_error_counter:
	 *
	 * This callback is used by drm-ras to query a specific error counter.
	 * Used for input check and to iterate all error counters in a node.
	 *
	 * Driver should expect query_error_counter() to be called with
	 * error_id from `error_counter_range.first` to
	 * `error_counter_range.last`.
	 *
	 * The @query_error_counter is a mandatory callback for
	 * error_counter_node.
	 *
	 * Returns: 0 on success,
	 *          -ENOENT when error_id is not supported as an indication that
	 *                  drm_ras should silently skip this entry. Used for
	 *                  supporting non-contiguous error ranges.
	 *                  Driver is responsible for maintaining the list of
	 *                  supported error IDs in the range of first to last.
	 *          Other negative values on errors that should terminate the
	 *          netlink query.
	 */
	int (*query_error_counter)(struct drm_ras_node *node, u32 error_id,
				   const char **name, u32 *val);

	/**
	 * @clear_error_counter:
	 *
	 * This callback is used by drm_ras to clear a specific error counter.
	 * Driver should implement this callback to support clearing error counters
	 * of a node.
	 *
	 * Returns: 0 on success, negative error code on failure.
	 */
	int (*clear_error_counter)(struct drm_ras_node *node, u32 error_id);

	/**
	 * @query_error_threshold:
	 *
	 * This callback is used by drm-ras to query error threshold of a
	 * specific counter.
	 *
	 * Driver should expect query_error_threshold() to be called with
	 * error_id from `error_counter_range.first` to
	 * `error_counter_range.last`.
	 *
	 * Returns: 0 on success, negative error code on failure.
	 */
	int (*query_error_threshold)(struct drm_ras_node *node, u32 error_id, const char **name,
				     u32 *threshold);

	/**
	 * @set_error_threshold:
	 *
	 * This callback is used by drm-ras to set error threshold of a specific
	 * counter.
	 *
	 * Driver should expect set_error_threshold() to be called with error_id
	 * from `error_counter_range.first` to `error_counter_range.last`.
	 * Driver is responsible for error threshold bounds checking.
	 *
	 * Returns: 0 on success, negative error code on failure.
	 */
	int (*set_error_threshold)(struct drm_ras_node *node, u32 error_id, u32 threshold);

	/**
	 * @query_retired_resource:
	 *
	 * This callback is used by drm-ras to enumerate retired resources of a
	 * %DRM_RAS_NODE_TYPE_RETIRED_RESOURCES node. It is called with @cursor
	 * and must return the next entry whose ordering key is greater than or
	 * equal to @cursor, setting @res->key to that entry's key. drm-ras
	 * resumes multi-part dumps from the returned key, so enumeration stays
	 * correct across concurrent insertion or removal.
	 *
	 * The @query_retired_resource is a mandatory callback for
	 * retired-resources nodes.
	 *
	 * Returns: 0 on success,
	 *          -ENOENT when no entry has a key >= @cursor, used as an
	 *                  indication that enumeration is complete.
	 *          Other negative values on errors that should terminate the
	 *          netlink query.
	 */
	int (*query_retired_resource)(struct drm_ras_node *node, u64 cursor,
				      struct drm_ras_retired_resource *res);

	/**
	 * @query_retired_summary:
	 *
	 * This optional callback fills a leading, type-specific capacity summary
	 * for a %DRM_RAS_NODE_TYPE_RETIRED_RESOURCES node (e.g. the maximum
	 * number of retirable pages). drm-ras emits it as the first entry of the
	 * retired-resources dump. The driver must set @res->is_summary, @res->type
	 * and the type-specific summary fields (e.g. vram_page.max_pages).
	 *
	 * Returns: 0 on success,
	 *          -ENOENT if the node has no summary to report,
	 *          other negative values on errors that terminate the query.
	 */
	int (*query_retired_summary)(struct drm_ras_node *node,
				     struct drm_ras_retired_resource *res);

	/** @priv: Driver private data */
	void *priv;
};

struct drm_device;

#if IS_ENABLED(CONFIG_DRM_RAS)
int drm_ras_node_register(struct drm_ras_node *node);
void drm_ras_node_unregister(struct drm_ras_node *node);
int drm_ras_nl_error_event(struct drm_ras_node *node, u32 error_id, const char *error_name,
			   u32 value);
#else
static inline int drm_ras_node_register(struct drm_ras_node *node) { return 0; }
static inline void drm_ras_node_unregister(struct drm_ras_node *node) { }
static inline int drm_ras_nl_error_event(struct drm_ras_node *node, u32 error_id,
					 const char *error_name, u32 value)
{ return 0; }
#endif

#endif
