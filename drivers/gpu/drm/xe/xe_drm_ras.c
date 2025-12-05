// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <drm/drm_managed.h>
#include <drm/drm_ras.h>
#include <linux/bitmap.h>

#include "xe_device.h"
#include "xe_drm_ras.h"

static const char * const errors[] = DRM_XE_RAS_ERROR_CLASS_NAMES;
static const char * const error_severity[] = DRM_XE_RAS_ERROR_SEVERITY_NAMES;

static int hw_query_error_counter(struct xe_drm_ras_counter *info,
				  u32 error_id, const char **name, u32 *val)
{
	if (error_id >= DRM_XE_RAS_ERROR_CLASS_MAX)
		return -EINVAL;

	if (!info[error_id].name)
		return -ENOENT;

	*name = info[error_id].name;
	*val = atomic64_read(&info[error_id].counter);

	return 0;
}

static int query_non_fatal_error_counters(struct drm_ras_node *ep,
					  u32 error_id, const char **name,
					  u32 *val)
{
	struct xe_device *xe = ep->priv;
	struct xe_drm_ras *ras = &xe->ras;
	struct xe_drm_ras_counter *info = ras->info[DRM_XE_RAS_ERROR_NONFATAL];

	return hw_query_error_counter(info, error_id, name, val);
}

static int query_fatal_error_counters(struct drm_ras_node *ep,
				      u32 error_id, const char **name,
				      u32 *val)
{
	struct xe_device *xe = ep->priv;
	struct xe_drm_ras *ras = &xe->ras;
	struct xe_drm_ras_counter *info = ras->info[DRM_XE_RAS_ERROR_FATAL];

	return hw_query_error_counter(info, error_id, name, val);
}

static int query_correctable_error_counters(struct drm_ras_node *ep,
					    u32 error_id, const char **name,
					    u32 *val)
{
	struct xe_device *xe = ep->priv;
	struct xe_drm_ras *ras = &xe->ras;
	struct xe_drm_ras_counter *info = ras->info[DRM_XE_RAS_ERROR_CORRECTABLE];

	return hw_query_error_counter(info, error_id, name, val);
}

static struct xe_drm_ras_counter *allocate_and_copy_counters(struct xe_device *xe,
							     int count)
{
	struct xe_drm_ras_counter *counter;
	int i;

	counter = drmm_kzalloc(&xe->drm, count * sizeof(struct xe_drm_ras_counter), GFP_KERNEL);
	if (!counter)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < count; i++) {
		if (!errors[i])
			continue;

		counter[i].name = errors[i];
		atomic64_set(&counter[i].counter, 0);
	}

	return counter;
}

static int assign_node_params(struct xe_device *xe, struct drm_ras_node *node,
			      const enum drm_xe_ras_error_severity severity)
{
	struct xe_drm_ras *ras = &xe->ras;
	int count = 0, ret = 0;

	count = DRM_XE_RAS_ERROR_CLASS_MAX;
	node->error_counter_range.first = DRM_XE_RAS_ERROR_CORE_COMPUTE;
	node->error_counter_range.last = DRM_XE_RAS_ERROR_CLASS_MAX - 1;

	ras->info[severity] = allocate_and_copy_counters(xe, count);
	if (IS_ERR(ras->info[severity]))
		return PTR_ERR(ras->info[severity]);

	switch (severity) {
	case DRM_XE_RAS_ERROR_CORRECTABLE:
		node->query_error_counter = query_correctable_error_counters;
		break;
	case DRM_XE_RAS_ERROR_NONFATAL:
		node->query_error_counter = query_non_fatal_error_counters;
		break;
	case DRM_XE_RAS_ERROR_FATAL:
		node->query_error_counter = query_fatal_error_counters;
		break;
	default:
		break;
	}

	return ret;
}

static int register_nodes(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct xe_drm_ras *ras = &xe->ras;
	const char *device_name;
	int i = 0, ret;

	device_name = kasprintf(GFP_KERNEL, "%04x:%02x:%02x.%d",
				pci_domain_nr(pdev->bus), pdev->bus->number,
				PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));

	for (i = 0; i < DRM_XE_RAS_ERROR_SEVERITY_MAX; i++) {
		struct drm_ras_node *node = &ras->node[i];

		node->device_name = device_name;
		node->node_name = error_severity[i];
		node->type = DRM_RAS_NODE_TYPE_ERROR_COUNTER;
		node->priv = xe;

		ret = assign_node_params(xe, node, i);
		if (ret)
			return ret;

		ret = drm_ras_node_register(node);
		if (ret) {
			drm_err(&xe->drm, "Failed to register drm ras tile node\n");
			return ret;
		}
	}

	return 0;
}

static void xe_drm_ras_unregister_nodes(void *arg)
{
	struct xe_device *xe = arg;
	struct xe_drm_ras *ras = &xe->ras;
	int i = 0;

	for (i = 0; i < DRM_XE_RAS_ERROR_SEVERITY_MAX; i++) {
		struct drm_ras_node *node = &ras->node[i];

		drm_ras_node_unregister(node);

		if (i == 0)
			kfree(node->device_name);
	}
}

/**
 * xe_drm_ras_allocate_nodes - Allocate drm ras nodes
 * @xe: xe device instance
 *
 * Allocate xe drm ras nodes for all error severities per device
 *
 * Return: 0 on success, error code on failure
 */
int xe_drm_ras_allocate_nodes(struct xe_device *xe)
{
	struct xe_drm_ras *ras = &xe->ras;
	struct drm_ras_node *node;
	int err;

	node = drmm_kzalloc(&xe->drm, DRM_XE_RAS_ERROR_SEVERITY_MAX * sizeof(struct drm_ras_node),
			    GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	ras->node = node;

	err = register_nodes(xe);
	if (err) {
		drm_err(&xe->drm, "Failed to register drm ras node\n");
		return err;
	}

	err = devm_add_action_or_reset(xe->drm.dev, xe_drm_ras_unregister_nodes, xe);
	if (err) {
		drm_err(&xe->drm, "Failed to add action for xe drm_ras\n");
		return err;
	}

	return 0;
}
