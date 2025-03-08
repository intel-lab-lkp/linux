// SPDX-License-Identifier: GPL-2.0-only
/*
 * processor thermal device platform temperature controls
 * Copyright (c) 2025, Intel Corporation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include "processor_thermal_device.h"

struct mmio_reg {
	int bits;
	u16 mask;
	u16 shift;
	u16 units;
};

#define MAX_ATTR_GROUP_NAME_LEN 32

struct ptc_data {
	u32 offset;
	struct attribute_group ptc_attr_group;
	struct attribute *ptc_attrs[3];
	struct device_attribute temperature_target_attr;
	struct device_attribute enable_attr;
	char group_name[MAX_ATTR_GROUP_NAME_LEN];
};

static const struct mmio_reg ptc_mmio_regs[] = {
	{ 8, 0xFF, 0, 500}, /* temperature_target, units 0.5C*/
	{ 1, 0x01, 8, 0}, /* enable */
	{ 3, 0x7, 9, 0}, /* gain */
	{ 8, 0xFF, 16, 0}, /* min_performance_level */
	{ 1, 0x1, 12, 0}, /* temperature_override_enable */
	{ 8, 0xFF, 24, 500}, /* temperature_override, units 0.5C */
};

/* Unique offset for each PTC instance */
static u32 ptc_offsets[] = {0x5B20, 0x5B28, 0x5B30};

/* These will represent sysfs attribute names */
static const char * const ptc_strings[] = {
	"temperature_target",
	"enable",
	NULL
};

#define PTC_SHOW(suffix)\
static ssize_t suffix##_show(struct device *dev,\
			     struct device_attribute *attr,\
			     char *buf)\
{\
	struct ptc_data *data = container_of(attr, struct ptc_data, suffix##_attr);\
	struct proc_thermal_device *proc_priv;\
	struct pci_dev *pdev = to_pci_dev(dev);\
	const struct mmio_reg *mmio_regs;\
	int ret, units;\
	u64 reg_val;\
\
	proc_priv = pci_get_drvdata(pdev);\
	mmio_regs = ptc_mmio_regs;\
	ret = match_string(ptc_strings, -1, attr->attr.name);\
	if (ret < 0)\
		return ret;\
	units = mmio_regs[ret].units;\
	reg_val = readq((void __iomem *) (proc_priv->mmio_base + data->offset));\
	ret = (reg_val >> mmio_regs[ret].shift) & mmio_regs[ret].mask;\
	if (units)\
		ret *= units;\
	return sysfs_emit(buf, "%d\n", ret);\
}

static void ptc_mmio_write(struct pci_dev *pdev, u32 offset, int index, u32 value)
{
	struct proc_thermal_device *proc_priv;
	u64 mask, reg_val;

	proc_priv = pci_get_drvdata(pdev);

	mask = GENMASK(ptc_mmio_regs[index].shift + ptc_mmio_regs[index].bits - 1,
		       ptc_mmio_regs[index].shift);
	reg_val = readq((void __iomem *) (proc_priv->mmio_base + offset));
	reg_val &= ~mask;
	reg_val |= (value << ptc_mmio_regs[index].shift);
	writeq(reg_val, (void __iomem *) (proc_priv->mmio_base + offset));
}

#define PTC_STORE(suffix)\
static ssize_t suffix##_store(struct device *dev,\
			      struct device_attribute *attr,\
			      const char *buf, size_t count)\
{\
	struct ptc_data *data = container_of(attr, struct ptc_data, suffix##_attr);\
	struct pci_dev *pdev = to_pci_dev(dev);\
	unsigned int input;\
	int ret;\
\
	ret = kstrtouint(buf, 10, &input);\
	if (ret)\
		return ret;\
	ret = match_string(ptc_strings, -1, attr->attr.name);\
	if (ret < 0)\
		return ret;\
	if (ptc_mmio_regs[ret].units)\
		input /= ptc_mmio_regs[ret].units;\
	if (input > ptc_mmio_regs[ret].mask)\
		return -EINVAL;\
	ptc_mmio_write(pdev, data->offset, ret, input);\
	return count;\
}

PTC_SHOW(temperature_target);
PTC_STORE(temperature_target);
PTC_SHOW(enable);
PTC_STORE(enable);

#define ptc_init_attribute(_name)\
	do {\
		sysfs_attr_init(&data->_name##_attr.attr);\
		data->_name##_attr.show = _name##_show;\
		data->_name##_attr.store = _name##_store;\
		data->_name##_attr.attr.name = #_name;\
		data->_name##_attr.attr.mode = 0644;\
	} while (0)

static int ptc_create_groups(struct pci_dev *pdev, int instance, struct ptc_data *data)
{
	int ret, index = 0;

	ptc_init_attribute(temperature_target);
	ptc_init_attribute(enable);

	data->ptc_attrs[index++] = &data->temperature_target_attr.attr;
	data->ptc_attrs[index++] = &data->enable_attr.attr;
	data->ptc_attrs[index] = NULL;

	snprintf(data->group_name, MAX_ATTR_GROUP_NAME_LEN,
		"platform_temperature_%d_control", instance);
	data->ptc_attr_group.name = data->group_name;
	data->ptc_attr_group.attrs = data->ptc_attrs;

	ret = sysfs_create_group(&pdev->dev.kobj, &data->ptc_attr_group);

	return ret;
}

#define PTC_MAX_INSTANCES	3
static struct ptc_data ptc_instance[PTC_MAX_INSTANCES];

int proc_thermal_ptc_add(struct pci_dev *pdev, struct proc_thermal_device *proc_priv)
{
	if (proc_priv->mmio_feature_mask & PROC_THERMAL_FEATURE_PTC) {
		int i;

		for (i = 0; i < PTC_MAX_INSTANCES; i++) {
			ptc_instance[i].offset = ptc_offsets[i];
			ptc_create_groups(pdev, i, &ptc_instance[i]);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(proc_thermal_ptc_add);

void proc_thermal_ptc_remove(struct pci_dev *pdev)
{
	struct proc_thermal_device *proc_priv = pci_get_drvdata(pdev);

	if (proc_priv->mmio_feature_mask & PROC_THERMAL_FEATURE_PTC) {
		int i;

		for (i = 0; i < PTC_MAX_INSTANCES; i++)
			sysfs_remove_group(&pdev->dev.kobj, &ptc_instance[i].ptc_attr_group);
	}
}
EXPORT_SYMBOL_GPL(proc_thermal_ptc_remove);

MODULE_IMPORT_NS("INT340X_THERMAL");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Processor Thermal PTC Interface");
