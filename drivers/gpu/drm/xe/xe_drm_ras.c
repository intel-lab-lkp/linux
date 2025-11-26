// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <drm/drm_managed.h>
#include <drm/drm_ras.h>
#include <drm/xe_drm.h>

#include "xe_device.h"
#include "xe_drm_ras.h"

#define ERR_INFO(index, _name) \
	[index] = { .name = _name, .counter = 0 }

static struct xe_drm_ras_counter error_info[] = {
	ERR_INFO(DRM_XE_GENL_CORE_COMPUTE, "GT Error"),
};

static int hw_query_error_counter(struct xe_drm_ras_counter *info,
				  u32 error_id, const char **name, u32 *val)
{
	*name = info[error_id].name;
	*val =  info[error_id].counter;

	return 0;
}

static int query_non_fatal_error_counters(struct drm_ras_node *ep,
					  u32 error_id, const char **name,
					  u32 *val)
{
	struct xe_device *xe = ep->priv;
	struct xe_drm_ras *ras = &xe->ras;
	struct xe_drm_ras_counter *info = ras->info[HARDWARE_ERROR_NONFATAL];

	if (error_id >= ARRAY_SIZE(error_info))
		return -EINVAL;

	if (!error_info[error_id].name)
		return -ENOENT;

	return hw_query_error_counter(info, error_id, name, val);
}

static int query_fatal_error_counters(struct drm_ras_node *ep,
				      u32 error_id, const char **name,
				      u32 *val)
{
	struct xe_device *xe = ep->priv;
	struct xe_drm_ras *ras = &xe->ras;
	struct xe_drm_ras_counter *info = ras->info[HARDWARE_ERROR_FATAL];

	if (error_id >= ARRAY_SIZE(error_info))
		return -EINVAL;

	if (!error_info[error_id].name)
		return -ENOENT;

	return hw_query_error_counter(info, error_id, name, val);
}

static int query_correctable_error_counters(struct drm_ras_node *ep,
					    u32 error_id, const char **name,
					    u32 *val)
{
	struct xe_device *xe = ep->priv;
	struct xe_drm_ras *ras = &xe->ras;
	struct xe_drm_ras_counter *info = ras->info[HARDWARE_ERROR_CORRECTABLE];

	if (error_id >= ARRAY_SIZE(error_info))
		return -EINVAL;

	if (!error_info[error_id].name)
		return -ENOENT;

	return hw_query_error_counter(info, error_id, name, val);
}

static struct xe_drm_ras_counter *allocate_and_copy_counters(struct xe_device *xe,
							     int count,
							     struct xe_drm_ras_counter *src)
{
	struct xe_drm_ras_counter *counter;

	counter = drmm_kzalloc(&xe->drm, count * sizeof(struct xe_drm_ras_counter), GFP_KERNEL);
	if (!counter)
		return ERR_PTR(-ENOMEM);

	memcpy(counter, src, count * sizeof(struct xe_drm_ras_counter));

	return counter;
}

static int assign_node_params(struct xe_device *xe, struct drm_ras_node *node,
			      enum hardware_error hw_err)
{
	struct xe_drm_ras *ras = &xe->ras;
	int count = 0, ret = 0;

	count = ARRAY_SIZE(error_info);
	node->error_counter_range.first = DRM_XE_GENL_CORE_COMPUTE;
	node->error_counter_range.last = count - 1;

	switch (hw_err) {
	case HARDWARE_ERROR_CORRECTABLE:
		ras->info[hw_err] = allocate_and_copy_counters(xe, count, error_info);
		if (IS_ERR(ras->info[hw_err]))
			return PTR_ERR(ras->info[hw_err]);
		node->query_error_counter = query_correctable_error_counters;
		break;
	case HARDWARE_ERROR_NONFATAL:
		ras->info[hw_err] = allocate_and_copy_counters(xe, count, error_info);
		if (IS_ERR(ras->info[hw_err]))
			return PTR_ERR(ras->info[hw_err]);
		node->query_error_counter = query_non_fatal_error_counters;
		break;
	case HARDWARE_ERROR_FATAL:
		ras->info[hw_err] = allocate_and_copy_counters(xe, count, error_info);
		if (IS_ERR(ras->info[hw_err]))
			return PTR_ERR(ras->info[hw_err]);
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

	for (i = 0; i < HARDWARE_ERROR_MAX; i++) {
		struct drm_ras_node *node = &ras->node[i];
		const char *hw_err_str = hw_error_to_str(i);
		const char *node_name;

		node_name = kasprintf(GFP_KERNEL, "%s-errors", hw_err_str);

		node->device_name = device_name;
		node->node_name = node_name;
		node->type = DRM_RAS_NODE_TYPE_ERROR_COUNTER;

		ret = assign_node_params(xe, node, i);
		if (ret) {
			kfree(node->node_name);
			return ret;
		}

		node->priv = xe;

		ret = drm_ras_node_register(node);
		if (ret) {
			drm_err(&xe->drm, "Failed to register drm ras tile node\n");
			kfree(node->node_name);
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

	for (i = 0; i < HARDWARE_ERROR_MAX; i++) {
		struct drm_ras_node *node = &ras->node[i];

		drm_ras_node_unregister(node);

		kfree(node->node_name);
		if (i == 0)
			kfree(node->device_name);
	}
}

/**
 * xe_drm_ras_allocate_nodes - Allocate drm ras nodes
 * @xe: xe device instance
 *
 * Allocate xe drm ras nodes for all errors in a tile
 *
 * Return: 0 on success, error code on failure
 */
int xe_drm_ras_allocate_nodes(struct xe_device *xe)
{
	struct drm_ras_node *node;
	int err;

	node = drmm_kzalloc(&xe->drm, HARDWARE_ERROR_MAX * sizeof(struct drm_ras_node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	xe->ras.node = node;

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
