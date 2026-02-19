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

static struct attribute *auxiliary_irq_attrs[] = {
	NULL
};

static const struct attribute_group auxiliary_irqs_group = {
	.name = "irqs",
	.attrs = auxiliary_irq_attrs,
};

/**
 * auxiliary_device_sysfs_irq_dir_init - initialize the IRQ sysfs directory
 * @auxdev: auxiliary bus device to initialize the sysfs directory.
 *
 * This function should be called by drivers to initialize the IRQ directory
 * before adding any IRQ sysfs entries. The driver is responsible for ensuring
 * this function is called only once and for handling any concurrency control
 * if needed.
 *
 * Drivers must call auxiliary_device_sysfs_irq_dir_destroy() to clean up when
 * done.
 *
 * Return: zero on success or an error code on failure.
 */
int auxiliary_device_sysfs_irq_dir_init(struct auxiliary_device *auxdev)
{
	int ret;

	ret = sysfs_create_group(&auxdev->dev.kobj, &auxiliary_irqs_group);
	if (ret)
		return ret;

	xa_init(&auxdev->sysfs.irqs);
	return 0;
}
EXPORT_SYMBOL_GPL(auxiliary_device_sysfs_irq_dir_init);

/**
 * auxiliary_device_sysfs_irq_dir_destroy - destroy the IRQ sysfs directory
 * @auxdev: auxiliary bus device to destroy the sysfs directory.
 *
 * This function should be called by drivers to clean up the IRQ directory
 * after all IRQ sysfs entries have been removed. The driver is responsible
 * for ensuring all IRQs are removed before calling this function.
 */
void auxiliary_device_sysfs_irq_dir_destroy(struct auxiliary_device *auxdev)
{
	xa_destroy(&auxdev->sysfs.irqs);
	sysfs_remove_group(&auxdev->dev.kobj, &auxiliary_irqs_group);
}
EXPORT_SYMBOL_GPL(auxiliary_device_sysfs_irq_dir_destroy);

/**
 * auxiliary_device_sysfs_irq_add - add a sysfs entry for the given IRQ
 * @auxdev: auxiliary bus device to add the sysfs entry.
 * @irq: The associated interrupt number.
 *
 * This function should be called after auxiliary device have successfully
 * received the irq. The driver must call auxiliary_device_sysfs_irq_dir_init()
 * before calling this function for the first time.
 * The driver is responsible to add a unique irq for the auxiliary device. The
 * driver can invoke this function from multiple thread context safely for
 * unique irqs of the auxiliary devices. The driver must not invoke this API
 * multiple times if the irq is already added previously.
 *
 * Return: zero on success or an error code on failure.
 */
int auxiliary_device_sysfs_irq_add(struct auxiliary_device *auxdev, int irq)
{
	struct auxiliary_irq_info *info __free(kfree) = NULL;
	struct device *dev = &auxdev->dev;
	int ret;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	sysfs_attr_init(&info->sysfs_attr.attr);
	snprintf(info->name, AUXILIARY_MAX_IRQ_NAME, "%d", irq);

	ret = xa_insert(&auxdev->sysfs.irqs, irq, info, GFP_KERNEL);
	if (ret)
		return ret;

	info->sysfs_attr.attr.name = info->name;
	ret = sysfs_add_file_to_group(&dev->kobj, &info->sysfs_attr.attr,
				      auxiliary_irqs_group.name);
	if (ret)
		goto sysfs_add_err;

	xa_store(&auxdev->sysfs.irqs, irq, no_free_ptr(info), GFP_KERNEL);
	return 0;

sysfs_add_err:
	xa_erase(&auxdev->sysfs.irqs, irq);
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
void auxiliary_device_sysfs_irq_remove(struct auxiliary_device *auxdev, int irq)
{
	struct auxiliary_irq_info *info __free(kfree) = xa_load(&auxdev->sysfs.irqs, irq);
	struct device *dev = &auxdev->dev;

	if (!info) {
		dev_err(&auxdev->dev, "IRQ %d doesn't exist\n", irq);
		return;
	}
	sysfs_remove_file_from_group(&dev->kobj, &info->sysfs_attr.attr,
				     auxiliary_irqs_group.name);
	xa_erase(&auxdev->sysfs.irqs, irq);
}
EXPORT_SYMBOL_GPL(auxiliary_device_sysfs_irq_remove);
