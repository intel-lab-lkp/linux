// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * eSPI slave-side device management and event notification
 *
 * Copyright (c) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/espi/espi.h>

static void espi_device_release(struct device *dev)
{
	struct espi_device *edev = to_espi_device(dev);

	fwnode_handle_put(dev_fwnode(dev));
	kfree(edev);
}

static const struct device_type espi_device_type = {
	.release = espi_device_release,
};

/**
 * espi_new_device - instantiate a new eSPI slave device
 * @ctrl: controller to which the device is attached
 * @info: board-level description of the device
 *
 * Creates and registers a new &struct espi_device on @ctrl.  The device
 * is named ``espi<bus>.<cs>`` and its modalias is set from
 * @info->type.
 *
 * Return: pointer to the new device, or an ERR_PTR() on failure.
 */
struct espi_device *espi_new_device(struct espi_controller *ctrl,
				    const struct espi_board_info *info)
{
	struct espi_device *edev;
	int ret;

	if (!ctrl || !info)
		return ERR_PTR(-EINVAL);
	if (ctrl->max_targets && info->cs >= ctrl->max_targets)
		return ERR_PTR(-EINVAL);

	edev = kzalloc_obj(*edev, GFP_KERNEL);
	if (!edev)
		return ERR_PTR(-ENOMEM);

	edev->ctrl = ctrl;
	edev->cs = info->cs;
	edev->platform_data = info->platform_data;
	strscpy(edev->modalias, info->type, sizeof(edev->modalias));
	INIT_LIST_HEAD(&edev->list);

	edev->dev.parent = &ctrl->dev;
	edev->dev.bus = &espi_bus_type;
	edev->dev.type = &espi_device_type;
	device_set_node(&edev->dev, fwnode_handle_get(info->fwnode));

	dev_set_name(&edev->dev, "espi%d.%u", ctrl->bus_num, info->cs);

	/*
	 * Add to the list before device_register() so the device is visible
	 * to any controller-side iteration as soon as the uevent fires.
	 */
	mutex_lock(&ctrl->device_list_lock);
	list_add_tail(&edev->list, &ctrl->device_list);
	mutex_unlock(&ctrl->device_list_lock);

	ret = device_register(&edev->dev);
	if (ret) {
		dev_err(&ctrl->dev, "failed to register device '%s': %d\n",
			dev_name(&edev->dev), ret);
		mutex_lock(&ctrl->device_list_lock);
		list_del_init(&edev->list);
		mutex_unlock(&ctrl->device_list_lock);
		put_device(&edev->dev);
		return ERR_PTR(ret);
	}

	return edev;
}
EXPORT_SYMBOL_GPL(espi_new_device);

/**
 * espi_remove_device - unregister and free an eSPI slave device
 * @edev: device to remove
 *
 * Removes @edev from the controller's device list and unregisters it
 * from the bus.  Must be called at most once per device.
 */
void espi_remove_device(struct espi_device *edev)
{
	struct espi_controller *ctrl;

	if (!edev)
		return;
	ctrl = edev->ctrl;

	mutex_lock(&ctrl->device_list_lock);
	if (WARN_ON(list_empty(&edev->list))) {
		mutex_unlock(&ctrl->device_list_lock);
		return;
	}
	list_del_init(&edev->list);
	mutex_unlock(&ctrl->device_list_lock);

	device_unregister(&edev->dev);
}
EXPORT_SYMBOL_GPL(espi_remove_device);

/**
 * espi_register_notifier - subscribe to eSPI hardware events
 * @ctrl: controller whose event chain to subscribe to
 * @nb:   notifier block to register
 *
 * Notifier callbacks are invoked from process context (threaded IRQ or
 * workqueue).  The @val argument passed to the callback is the
 * &enum espi_event_type value; @data points to the &struct espi_event.
 *
 * Return: 0 on success, negative errno on failure.
 */
int espi_register_notifier(struct espi_controller *ctrl,
			   struct notifier_block *nb)
{
	if (!ctrl || !nb)
		return -EINVAL;
	return blocking_notifier_chain_register(&ctrl->notifier_list, nb);
}
EXPORT_SYMBOL_GPL(espi_register_notifier);

/**
 * espi_unregister_notifier - unsubscribe from eSPI hardware events
 * @ctrl: controller whose event chain to unsubscribe from
 * @nb:   notifier block to unregister
 *
 * Return: 0 on success, negative errno on failure.
 */
int espi_unregister_notifier(struct espi_controller *ctrl,
			     struct notifier_block *nb)
{
	if (!ctrl || !nb)
		return -EINVAL;
	return blocking_notifier_chain_unregister(&ctrl->notifier_list, nb);
}
EXPORT_SYMBOL_GPL(espi_unregister_notifier);

/**
 * espi_notify_event - deliver a hardware event to all registered listeners
 * @ctrl:  controller on which the event occurred
 * @event: event descriptor; @event->ctrl is set by this function
 *
 * Must be called from process context (threaded IRQ or workqueue), never
 * from hard-IRQ context and never with @ctrl->lock held.
 *
 * Return: a NOTIFY_* value, not an errno.  Callers that need to map this
 * to an errno should use notifier_to_errno().
 */
int espi_notify_event(struct espi_controller *ctrl, struct espi_event *event)
{
	if (!ctrl || !event)
		return notifier_from_errno(-EINVAL);
	event->ctrl = ctrl;
	return blocking_notifier_call_chain(&ctrl->notifier_list,
					    (unsigned long)event->type, event);
}
EXPORT_SYMBOL_GPL(espi_notify_event);

MODULE_AUTHOR("Krishnamoorthi M <krishnamoorthi.m@amd.com>");
MODULE_DESCRIPTION("eSPI slave-side device management");
MODULE_LICENSE("GPL");
