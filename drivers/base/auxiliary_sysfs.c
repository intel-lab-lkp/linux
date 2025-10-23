// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */

#include <linux/auxiliary_bus.h>
#include <linux/slab.h>

#define AUXILIARY_MAX_IRQ_NAME 11

struct auxiliary_irq_info {
	struct device_attribute sysfs_attr;
	char name[AUXILIARY_MAX_IRQ_NAME];
};

static struct attribute auxiliary_irq_attr = {
	.mode = 0,
	.name = "DUMMY",
};

static struct attribute *auxiliary_irq_attrs[] = {
	[0] = &auxiliary_irq_attr,
	[1] = NULL,
};

static bool auxiliary_irq_dir_group_visible(struct kobject *kobj)
{
	struct auxiliary_device *auxdev;
	struct device *dev;

	dev = container_of(kobj, struct device, kobj);
	auxdev = container_of(dev, struct auxiliary_device, dev);

	return !xa_empty(&auxdev->sysfs.irqs);
}

DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(auxiliary_irq_dir);

static const struct attribute_group auxiliary_irqs_group = {
	.name = "irqs",
	.attrs = auxiliary_irq_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(auxiliary_irq_dir),
};

void auxiliary_bus_irq_dir_res_remove(struct auxiliary_device *auxdev)
{
	struct device *dev = &auxdev->dev;

	sysfs_remove_group(&dev->kobj, &auxiliary_irqs_group);
	xa_destroy(&auxdev->sysfs.irqs);
	mutex_destroy(&auxdev->sysfs.lock);
}

int auxiliary_bus_irq_dir_res_probe(struct auxiliary_device *auxdev)
{
	struct device *dev = &auxdev->dev;

	mutex_init(&auxdev->sysfs.lock);
	xa_init(&auxdev->sysfs.irqs);
	return sysfs_create_group(&dev->kobj, &auxiliary_irqs_group);
}

static struct auxiliary_irq_info *auxiliary_irq_info_init(int irq)
{
	struct auxiliary_irq_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return NULL;

	sysfs_attr_init(&info->sysfs_attr.attr);
	snprintf(info->name, AUXILIARY_MAX_IRQ_NAME, "%d", irq);
	info->sysfs_attr.attr.name = info->name;

	return info;
}

static void auxiliary_irq_info_destroy(struct auxiliary_irq_info *info)
{
	kfree(info);
}

/**
 * auxiliary_device_sysfs_irq_add - add a sysfs entry for the given IRQ
 * @auxdev: auxiliary bus device to add the sysfs entry.
 * @irq: The associated interrupt number.
 *
 * This function should be called after auxiliary device have successfully
 * received the irq.
 * The driver is responsible to add a unique irq for the auxiliary device. The
 * driver can invoke this function from multiple thread context safely for
 * unique irqs of the auxiliary devices. The driver must not invoke this API
 * multiple times if the irq is already added previously.
 *
 * Return: zero on success or an error code on failure.
 */
int auxiliary_device_sysfs_irq_add(struct auxiliary_device *auxdev, int irq)
{
	struct device *dev = &auxdev->dev;
	struct auxiliary_irq_info *info;
	bool sysfs_add_error = false;
	int ret;

	info = auxiliary_irq_info_init(irq);
	if (!info)
		return -ENOMEM;

	mutex_lock(&auxdev->sysfs.lock);
	ret = xa_insert(&auxdev->sysfs.irqs, irq, info, GFP_KERNEL);
	if (ret)
		goto unlock;

	ret = sysfs_update_group(&dev->kobj, &auxiliary_irqs_group);
	if (ret)
		goto irq_erase;

	ret = sysfs_add_file_to_group(&dev->kobj, &info->sysfs_attr.attr,
				      auxiliary_irqs_group.name);
	if (ret) {
		sysfs_add_error = true;
		goto irq_erase;
	}

	mutex_unlock(&auxdev->sysfs.lock);
	return 0;

irq_erase:
	xa_erase(&auxdev->sysfs.irqs, irq);
	if (sysfs_add_error)
		sysfs_update_group(&dev->kobj, &auxiliary_irqs_group);
unlock:
	mutex_unlock(&auxdev->sysfs.lock);
	auxiliary_irq_info_destroy(info);
	return ret;
}
EXPORT_SYMBOL_GPL(auxiliary_device_sysfs_irq_add);

/**
 * auxiliary_device_sysfs_irq_remove - remove a sysfs entry for the given IRQ
 * @auxdev: auxiliary bus device to add the sysfs entry.
 * @irq: the IRQ to remove.
 *
 * This function should be called to remove an IRQ sysfs entry.
 * The driver must invoke this API when IRQ is released by the device.
 */
int auxiliary_device_sysfs_irq_remove(struct auxiliary_device *auxdev, int irq)
{
	struct device *dev = &auxdev->dev;
	struct auxiliary_irq_info *info;
	int err;

	mutex_lock(&auxdev->sysfs.lock);
	info = xa_load(&auxdev->sysfs.irqs, irq);
	if (!info) {
		mutex_unlock(&auxdev->sysfs.lock);
		dev_err(&auxdev->dev, "IRQ %d doesn't exist\n", irq);
		return -ENOMEM;
	}

	sysfs_remove_file_from_group(&dev->kobj, &info->sysfs_attr.attr,
				     auxiliary_irqs_group.name);
	xa_erase(&auxdev->sysfs.irqs, irq);
	err = sysfs_update_group(&dev->kobj, &auxiliary_irqs_group);
	if (err)
		dev_err(&auxdev->dev,
			"Failed to update IRQs group, irq %d\n", irq);

	mutex_unlock(&auxdev->sysfs.lock);
	auxiliary_irq_info_destroy(info);
	return err;
}
EXPORT_SYMBOL_GPL(auxiliary_device_sysfs_irq_remove);
