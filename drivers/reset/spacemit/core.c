// SPDX-License-Identifier: GPL-2.0-only

/* SpacemiT reset driver core */

#include <linux/container_of.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/types.h>

#include "core.h"

static int spacemit_reset_update(struct reset_controller_dev *rcdev,
				 unsigned long id, bool assert)
{
	struct ccu_reset_controller *controller;
	const struct ccu_reset_data *data;
	u32 mask;
	u32 val;

	controller = container_of(rcdev, struct ccu_reset_controller, rcdev);
	data = &controller->data->reset_data[id];
	mask = data->assert_mask | data->deassert_mask;
	val = assert ? data->assert_mask : data->deassert_mask;

	return regmap_update_bits(controller->regmap, data->offset, mask, val);
}

static int spacemit_reset_assert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return spacemit_reset_update(rcdev, id, true);
}

static int spacemit_reset_deassert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	return spacemit_reset_update(rcdev, id, false);
}

static const struct reset_control_ops spacemit_reset_control_ops = {
	.assert		= spacemit_reset_assert,
	.deassert	= spacemit_reset_deassert,
};

/**
 * spacemit_reset_controller_register() - register a reset controller
 * @dev:	Device that's registering the controller
 * @controller:	SpacemiT CCU reset controller structure
 */
int spacemit_reset_controller_register(struct device *dev,
				       struct ccu_reset_controller *controller)
{
	struct reset_controller_dev *rcdev = &controller->rcdev;

	rcdev->ops = &spacemit_reset_control_ops;
	rcdev->owner = THIS_MODULE;
	rcdev->of_node = dev->of_node;
	rcdev->nr_resets = controller->data->count;

	return devm_reset_controller_register(dev, &controller->rcdev);
}
