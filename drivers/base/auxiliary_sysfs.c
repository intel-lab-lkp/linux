// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */

#include <linux/auxiliary_bus.h>
#include <linux/slab.h>

struct auxiliary_irq_info {
	struct device_attribute sysfs_attr;
};

static struct attribute *auxiliary_irq_attrs[] = {
	NULL
};

static const struct attribute_group auxiliary_irqs_group = {
	.name = "irqs",
	.attrs = auxiliary_irq_attrs,
};

static int auxiliary_irq_dir_prepare(struct auxiliary_device *auxdev)
{
	int ret = 0;

	mutex_lock(&auxdev->lock);
	if (auxdev->dir_exists)
		goto unlock;

	xa_init(&auxdev->irqs);
	ret = devm_device_add_group(&auxdev->dev, &auxiliary_irqs_group);
	if (!ret)
		auxdev->dir_exists = 1;

unlock:
	mutex_unlock(&auxdev->lock);
	return ret;
}

/**
 * auxiliary_device_sysfs_irq_add - add a sysfs entry for the given IRQ
 * @auxdev: auxiliary bus device to add the sysfs entry.
 * @irq: The associated interrupt number.
 *
 * This function should be called after auxiliary device have successfully
 * received the irq.
 *
 * Return: zero on success or an error code on failure.
 */
int auxiliary_device_sysfs_irq_add(struct auxiliary_device *auxdev, int irq)
{
	struct device *dev = &auxdev->dev;
	struct auxiliary_irq_info *info;
	int ret;

	ret = auxiliary_irq_dir_prepare(auxdev);
	if (ret)
		return ret;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	sysfs_attr_init(&info->sysfs_attr.attr);
	info->sysfs_attr.attr.name = kasprintf(GFP_KERNEL, "%d", irq);
	if (!info->sysfs_attr.attr.name) {
		ret = -ENOMEM;
		goto name_err;
	}

	ret = xa_insert(&auxdev->irqs, irq, info, GFP_KERNEL);
	if (ret)
		goto auxdev_xa_err;

	ret = sysfs_add_file_to_group(&dev->kobj, &info->sysfs_attr.attr,
				      auxiliary_irqs_group.name);
	if (ret)
		goto sysfs_add_err;

	return 0;

sysfs_add_err:
	xa_erase(&auxdev->irqs, irq);
auxdev_xa_err:
	kfree(info->sysfs_attr.attr.name);
name_err:
	kfree(info);
	return ret;
}
EXPORT_SYMBOL_GPL(auxiliary_device_sysfs_irq_add);

/**
 * auxiliary_device_sysfs_irq_remove - remove a sysfs entry for the given IRQ
 * @auxdev: auxiliary bus device to add the sysfs entry.
 * @irq: the IRQ to remove.
 *
 * This function should be called to remove an IRQ sysfs entry.
 */
void auxiliary_device_sysfs_irq_remove(struct auxiliary_device *auxdev, int irq)
{
	struct auxiliary_irq_info *info = xa_load(&auxdev->irqs, irq);
	struct device *dev = &auxdev->dev;

	sysfs_remove_file_from_group(&dev->kobj, &info->sysfs_attr.attr,
				     auxiliary_irqs_group.name);
	xa_erase(&auxdev->irqs, irq);
	kfree(info->sysfs_attr.attr.name);
	kfree(info);
}
EXPORT_SYMBOL_GPL(auxiliary_device_sysfs_irq_remove);
