/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/drm_ras.yaml */
/* YNL-GEN uapi header */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#ifndef _UAPI_LINUX_DRM_RAS_H
#define _UAPI_LINUX_DRM_RAS_H

#define DRM_RAS_FAMILY_NAME	"drm-ras"
#define DRM_RAS_FAMILY_VERSION	1

/*
 * Type of the node. error-counter nodes expose reliability counters for a
 * hardware/software component. retired-resources nodes enumerate hardware
 * resources (e.g. VRAM pages) that have been permanently taken out of service.
 */
enum drm_ras_node_type {
	DRM_RAS_NODE_TYPE_ERROR_COUNTER = 1,
	DRM_RAS_NODE_TYPE_RETIRED_RESOURCES,
};

/*
 * Status of a retired resource entry. retired means the resource is
 * permanently reserved and out of service; pending means retirement is queued
 * but the reservation is not yet complete; failed means the reservation failed
 * and the resource may still be in use.
 */
enum drm_ras_retired_resource_status {
	DRM_RAS_RETIRED_RESOURCE_STATUS_RETIRED,
	DRM_RAS_RETIRED_RESOURCE_STATUS_PENDING,
	DRM_RAS_RETIRED_RESOURCE_STATUS_FAILED,
};

/*
 * Type of a retired resource entry. The type selects which type-specific
 * nested attribute is present. New hardware resource types can be added here,
 * each carrying its own nested attribute set, without affecting existing
 * types. vram-page describes a VRAM page by device address and size.
 */
enum drm_ras_retired_resource_type {
	DRM_RAS_RETIRED_RESOURCE_TYPE_VRAM_PAGE = 1,
};

enum {
	DRM_RAS_A_NODE_ATTRS_NODE_ID = 1,
	DRM_RAS_A_NODE_ATTRS_DEVICE_NAME,
	DRM_RAS_A_NODE_ATTRS_NODE_NAME,
	DRM_RAS_A_NODE_ATTRS_NODE_TYPE,

	__DRM_RAS_A_NODE_ATTRS_MAX,
	DRM_RAS_A_NODE_ATTRS_MAX = (__DRM_RAS_A_NODE_ATTRS_MAX - 1)
};

enum {
	DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID = 1,
	DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID,
	DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_NAME,
	DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_VALUE,
	DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_THRESHOLD,

	__DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX,
	DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX = (__DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX - 1)
};

enum {
	DRM_RAS_A_ERROR_EVENT_ATTRS_DEVICE_NAME = 1,
	DRM_RAS_A_ERROR_EVENT_ATTRS_NODE_ID,
	DRM_RAS_A_ERROR_EVENT_ATTRS_NODE_NAME,
	DRM_RAS_A_ERROR_EVENT_ATTRS_ERROR_ID,
	DRM_RAS_A_ERROR_EVENT_ATTRS_ERROR_NAME,
	DRM_RAS_A_ERROR_EVENT_ATTRS_ERROR_VALUE,

	__DRM_RAS_A_ERROR_EVENT_ATTRS_MAX,
	DRM_RAS_A_ERROR_EVENT_ATTRS_MAX = (__DRM_RAS_A_ERROR_EVENT_ATTRS_MAX - 1)
};

enum {
	DRM_RAS_A_RETIRED_RESOURCE_ATTRS_NODE_ID = 1,
	DRM_RAS_A_RETIRED_RESOURCE_ATTRS_VRAM_PAGE,

	__DRM_RAS_A_RETIRED_RESOURCE_ATTRS_MAX,
	DRM_RAS_A_RETIRED_RESOURCE_ATTRS_MAX = (__DRM_RAS_A_RETIRED_RESOURCE_ATTRS_MAX - 1)
};

enum {
	DRM_RAS_A_VRAM_PAGE_ATTRS_ADDRESS = 1,
	DRM_RAS_A_VRAM_PAGE_ATTRS_SIZE,
	DRM_RAS_A_VRAM_PAGE_ATTRS_MAX_PAGES,
	DRM_RAS_A_VRAM_PAGE_ATTRS_STATUS,
	DRM_RAS_A_VRAM_PAGE_ATTRS_PAD,

	__DRM_RAS_A_VRAM_PAGE_ATTRS_MAX,
	DRM_RAS_A_VRAM_PAGE_ATTRS_MAX = (__DRM_RAS_A_VRAM_PAGE_ATTRS_MAX - 1)
};

enum {
	DRM_RAS_CMD_LIST_NODES = 1,
	DRM_RAS_CMD_GET_ERROR_COUNTER,
	DRM_RAS_CMD_CLEAR_ERROR_COUNTER,
	DRM_RAS_CMD_ERROR_EVENT,
	DRM_RAS_CMD_GET_ERROR_THRESHOLD,
	DRM_RAS_CMD_SET_ERROR_THRESHOLD,
	DRM_RAS_CMD_GET_RETIRED_RESOURCES,

	__DRM_RAS_CMD_MAX,
	DRM_RAS_CMD_MAX = (__DRM_RAS_CMD_MAX - 1)
};

#define DRM_RAS_MCGRP_ERROR_REPORT	"error-report"

#endif /* _UAPI_LINUX_DRM_RAS_H */
