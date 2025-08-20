// SPDX-License-Identifier: GPL-2.0+
/*
 * Platform driver for the Embedded Controller (EC) of Ayaneo devices. Handles
 * hwmon (fan speed, fan control), battery charge limits, and magic module
 * control (connected modules, controller disconnection).
 *
 * Copyright (C) 2025 Antheas Kapenekakis <lkml@antheas.dev>
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <acpi/battery.h>

#include "firmware_attributes_class.h"

#define DRIVER_NAME "ayaneo-ec"

#define AYANEO_PWM_ENABLE_REG	 0x4A
#define AYANEO_PWM_REG		 0x4B
#define AYANEO_PWM_MODE_AUTO	 0x00
#define AYANEO_PWM_MODE_MANUAL	 0x01

#define AYANEO_FAN_REG		 0x76

#define EC_CHARGE_CONTROL_BEHAVIOURS                         \
	(BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |           \
	 BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE))
#define AYANEO_CHARGE_REG		0x1e
#define AYANEO_CHARGE_VAL_AUTO		0xaa
#define AYANEO_CHARGE_VAL_INHIBIT	0x55

#define AYANEO_POWER_REG	0x2d
#define AYANEO_POWER_OFF	0xfe
#define AYANEO_POWER_ON		0xff
#define AYANEO_MODULE_REG	0x2f
#define AYANEO_MODULE_LEFT	BIT(0)
#define AYANEO_MODULE_RIGHT	BIT(1)

enum ayaneo_fw_attr_id {
	AYANEO_ATTR_CONTROLLER_MODULES,
	AYANEO_ATTR_CONTROLLER_POWER,
};

static const char *const ayaneo_fw_attr_name[] = {
	[AYANEO_ATTR_CONTROLLER_MODULES] = "controller_modules",
	[AYANEO_ATTR_CONTROLLER_POWER] = "controller_power",
};

static const char *const ayaneo_fw_attr_desc[] = {
	[AYANEO_ATTR_CONTROLLER_MODULES] =
		"Which controller Magic Modules are connected (none, left, right, both)",
	[AYANEO_ATTR_CONTROLLER_POWER] = "Controller power state (on, off)",
};

#define AYANEO_ATTR_ENUM_MAX_ATTRS 7
#define AYANEO_ATTR_LANGUAGE_CODE "en_US.UTF-8"

struct ayaneo_ec_quirk {
	bool has_fan_control;
	bool has_charge_control;
	bool has_magic_modules;
	bool has_controller_power;
};

struct ayaneo_ec_platform_data {
	struct platform_device *pdev;
	struct ayaneo_ec_quirk *quirks;
	struct acpi_battery_hook battery_hook;
	struct device *fw_attrs_dev;
	struct kset *fw_attrs_kset;
};

struct ayaneo_fw_attr {
	struct ayaneo_ec_platform_data *data;
	enum ayaneo_fw_attr_id fw_attr_id;
	struct attribute_group attr_group;
	struct kobj_attribute display_name;
	struct kobj_attribute current_value;
};

static const struct ayaneo_ec_quirk quirk_fan = {
	.has_fan_control = true,
};

static const struct ayaneo_ec_quirk quirk_charge_limit = {
	.has_fan_control = true,
	.has_charge_control = true,
};

static const struct ayaneo_ec_quirk ayaneo3 = {
	.has_fan_control = true,
	.has_charge_control = true,
	.has_magic_modules = true,
	.has_controller_power = true,
};

static const struct dmi_system_id dmi_table[] = {

	{
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_MATCH(DMI_BOARD_NAME, "AYANEO 2"),
		},
		.driver_data = (void *)&quirk_fan,
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_MATCH(DMI_BOARD_NAME, "FLIP"),
		},
		.driver_data = (void *)&quirk_fan,
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_MATCH(DMI_BOARD_NAME, "GEEK"),
		},
		.driver_data = (void *)&quirk_fan,
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AIR"),
		},
		.driver_data = (void *)&quirk_charge_limit,
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AIR 1S"),
		},
		.driver_data = (void *)&quirk_charge_limit,
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AB05-Mendocino"),
		},
		.driver_data = (void *)&quirk_charge_limit,
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AIR Pro"),
		},
		.driver_data = (void *)&quirk_charge_limit,
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "KUN"),
		},
		.driver_data = (void *)&quirk_charge_limit,
	},
	{
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AYANEO 3"),
		},
		.driver_data = (void *)&ayaneo3,
	},
	{},
};

/* Callbacks for hwmon interface */
static umode_t ayaneo_ec_hwmon_is_visible(const void *drvdata,
				       enum hwmon_sensor_types type, u32 attr,
				       int channel)
{
	switch (type) {
	case hwmon_fan:
		return 0444;
	case hwmon_pwm:
		return 0644;
	default:
		return 0;
	}
}

static int ayaneo_ec_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	u8 tmp;
	int ret;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			ret = ec_read(AYANEO_FAN_REG, &tmp);
			if (ret)
				return ret;
			*val = tmp << 8;
			ret = ec_read(AYANEO_FAN_REG + 1, &tmp);
			if (ret)
				return ret;
			*val += tmp;
			return 0;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			ret = ec_read(AYANEO_PWM_REG, &tmp);
			if (ret)
				return ret;
			*val = (255 * tmp) / 100;
			if (*val < 0 || *val > 255)
				return -EINVAL;
			return 0;
		case hwmon_pwm_enable:
			ret = ec_read(AYANEO_PWM_ENABLE_REG, &tmp);
			if (ret)
				return ret;
			if (tmp == AYANEO_PWM_MODE_MANUAL)
				*val = 1;
			else
				*val = 2;
			return 0;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static int ayaneo_ec_write(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			if (val == 1)
				return ec_write(AYANEO_PWM_ENABLE_REG,
						AYANEO_PWM_MODE_MANUAL);
			else if (val == 2)
				return ec_write(AYANEO_PWM_ENABLE_REG,
						AYANEO_PWM_MODE_AUTO);
			else
				return -EINVAL;
		case hwmon_pwm_input:
			if (val < 0 || val > 255)
				return -EINVAL;
			return ec_write(AYANEO_PWM_REG, (val * 100) / 255);
		default:
			break;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops ayaneo_ec_hwmon_ops = {
	.is_visible = ayaneo_ec_hwmon_is_visible,
	.read = ayaneo_ec_read,
	.write = ayaneo_ec_write,
};

static const struct hwmon_channel_info *const ayaneo_ec_sensors[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL,
};

static const struct hwmon_chip_info ayaneo_ec_chip_info = {
	.ops = &ayaneo_ec_hwmon_ops,
	.info = ayaneo_ec_sensors,
};

static int ayaneo_psy_ext_get_prop(struct power_supply *psy,
				const struct power_supply_ext *ext,
				void *data,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int ret;
	u8 tmp;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		ret = ec_read(AYANEO_CHARGE_REG, &tmp);
		if (ret)
			return ret;

		if (tmp == AYANEO_CHARGE_VAL_INHIBIT)
			val->intval = POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE;
		else
			val->intval = POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ayaneo_psy_ext_set_prop(struct power_supply *psy,
				const struct power_supply_ext *ext,
				void *data,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	u8 raw_val;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		switch (val->intval) {
		case POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO:
			raw_val = AYANEO_CHARGE_VAL_AUTO;
			break;
		case POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE:
			raw_val = AYANEO_CHARGE_VAL_INHIBIT;
			break;
		default:
			return -EINVAL;
		}
		return ec_write(AYANEO_CHARGE_REG, raw_val);
	default:
		return -EINVAL;
	}
}

static int ayaneo_psy_prop_is_writeable(struct power_supply *psy,
				     const struct power_supply_ext *ext,
				     void *data,
				     enum power_supply_property psp)
{
	return true;
}

static const enum power_supply_property ayaneo_psy_ext_props[] = {
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
};

static const struct power_supply_ext ayaneo_psy_ext = {
	.name			= "ayaneo-charge-control",
	.properties		= ayaneo_psy_ext_props,
	.num_properties		= ARRAY_SIZE(ayaneo_psy_ext_props),
	.charge_behaviours	= EC_CHARGE_CONTROL_BEHAVIOURS,
	.get_property		= ayaneo_psy_ext_get_prop,
	.set_property		= ayaneo_psy_ext_set_prop,
	.property_is_writeable	= ayaneo_psy_prop_is_writeable,
};

static int ayaneo_add_battery(struct power_supply *battery,
			   struct acpi_battery_hook *hook)
{
	struct ayaneo_ec_platform_data *data =
		container_of(hook, struct ayaneo_ec_platform_data, battery_hook);

	return power_supply_register_extension(battery, &ayaneo_psy_ext,
					       &data->pdev->dev, NULL);
}

static int ayaneo_remove_battery(struct power_supply *battery,
			      struct acpi_battery_hook *hook)
{
	power_supply_unregister_extension(battery, &ayaneo_psy_ext);
	return 0;
}

static void ayaneo_kset_unregister(void *data)
{
	struct kset *kset = data;

	kset_unregister(kset);
}

static void ayaneo_fw_attrs_dev_unregister(void *data)
{
	struct device *fw_attrs_dev = data;

	device_unregister(fw_attrs_dev);
}

static ssize_t display_name_language_code_show(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       char *buf)
{
	return sysfs_emit(buf, "%s\n", AYANEO_ATTR_LANGUAGE_CODE);
}

static struct kobj_attribute fw_attr_display_name_language_code =
	__ATTR_RO(display_name_language_code);

static ssize_t display_name_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	struct ayaneo_fw_attr *fw_attr =
		container_of(attr, struct ayaneo_fw_attr, display_name);

	return sysfs_emit(buf, "%s\n", ayaneo_fw_attr_desc[fw_attr->fw_attr_id]);
}

static ssize_t current_value_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct ayaneo_fw_attr *fw_attr =
		container_of(attr, struct ayaneo_fw_attr, current_value);
	bool left, right;
	char *out;
	int ret;
	u8 tmp;

	switch (fw_attr->fw_attr_id) {
	case AYANEO_ATTR_CONTROLLER_MODULES:
		ret = ec_read(AYANEO_MODULE_REG, &tmp);
		if (ret)
			return ret;
		left = !(tmp & AYANEO_MODULE_LEFT);
		right = !(tmp & AYANEO_MODULE_RIGHT);

		if (left && right)
			out = "both";
		else if (left)
			out = "left";
		else if (right)
			out = "right";
		else
			out = "none";

		return sysfs_emit(buf, "%s\n", out);
	case AYANEO_ATTR_CONTROLLER_POWER:
		ret = ec_read(AYANEO_POWER_REG, &tmp);
		if (ret)
			return ret;

		if (tmp == AYANEO_POWER_OFF)
			out = "off";
		else
			out = "on";

		return sysfs_emit(buf, "%s\n", out);
	}
	return -EINVAL;
}

static ssize_t current_value_store(struct kobject *kobj,
				   struct kobj_attribute *attr, const char *buf,
				   size_t count)
{
	struct ayaneo_fw_attr *fw_attr =
		container_of(attr, struct ayaneo_fw_attr, current_value);
	int ret;

	switch (fw_attr->fw_attr_id) {
	case AYANEO_ATTR_CONTROLLER_POWER:
		if (sysfs_streq(buf, "on"))
			ret = ec_write(AYANEO_POWER_REG, AYANEO_POWER_ON);
		else if (sysfs_streq(buf, "off"))
			ret = ec_write(AYANEO_POWER_REG, AYANEO_POWER_OFF);
		if (ret)
			return ret;
		return count;
	case AYANEO_ATTR_CONTROLLER_MODULES:
		return -EINVAL;
	}
	return -EINVAL;
}

static ssize_t type_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "string\n");
}

static struct kobj_attribute fw_attr_type_string = {
	.attr = { .name = "type", .mode = 0444 },
	.show = type_show,
};

static int ayaneo_fw_attr_init(struct ayaneo_ec_platform_data *data,
			       const enum ayaneo_fw_attr_id fw_attr_id,
			       bool read_only)
{
	struct ayaneo_fw_attr *fw_attr;
	struct attribute **attrs;
	int idx = 0;

	fw_attr = devm_kzalloc(&data->pdev->dev, sizeof(*fw_attr), GFP_KERNEL);
	if (!fw_attr)
		return -ENOMEM;

	attrs = devm_kcalloc(&data->pdev->dev, AYANEO_ATTR_ENUM_MAX_ATTRS + 1,
			     sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return -ENOMEM;

	fw_attr->data = data;
	fw_attr->fw_attr_id = fw_attr_id;
	fw_attr->attr_group.name = ayaneo_fw_attr_name[fw_attr_id];
	fw_attr->attr_group.attrs = attrs;

	attrs[idx++] = &fw_attr_type_string.attr;
	attrs[idx++] = &fw_attr_display_name_language_code.attr;

	sysfs_attr_init(&fw_attr->display_name.attr);
	fw_attr->display_name.attr.name = "display_name";
	fw_attr->display_name.attr.mode = 0444;
	fw_attr->display_name.show = display_name_show;
	attrs[idx++] = &fw_attr->display_name.attr;

	sysfs_attr_init(&fw_attr->current_value.attr);
	fw_attr->current_value.attr.name = "current_value";
	fw_attr->current_value.attr.mode = read_only ? 0444 : 0644;
	fw_attr->current_value.show = current_value_show;
	fw_attr->current_value.store = current_value_store;
	attrs[idx++] = &fw_attr->current_value.attr;

	attrs[idx] = NULL;
	return sysfs_create_group(&data->fw_attrs_kset->kobj,
				  &fw_attr->attr_group);
}

static int ayaneo_ec_probe(struct platform_device *pdev)
{
	const struct dmi_system_id *dmi_entry;
	struct ayaneo_ec_platform_data *data;
	struct device *hwdev;
	int ret;

	dmi_entry = dmi_first_match(dmi_table);
	if (!dmi_entry)
		return -ENODEV;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->pdev = pdev;
	data->quirks = dmi_entry->driver_data;
	platform_set_drvdata(pdev, data);

	if (data->quirks->has_fan_control) {
		hwdev = devm_hwmon_device_register_with_info(
			&pdev->dev, "ayaneo_ec", NULL, &ayaneo_ec_chip_info, NULL);
		if (IS_ERR(hwdev))
			return PTR_ERR(hwdev);
	}

	if (data->quirks->has_charge_control) {
		data->battery_hook.add_battery = ayaneo_add_battery;
		data->battery_hook.remove_battery = ayaneo_remove_battery;
		data->battery_hook.name = "Ayaneo Battery";
		ret = devm_battery_hook_register(&pdev->dev, &data->battery_hook);
		if (ret)
			return ret;
	}

	if (data->quirks->has_magic_modules || data->quirks->has_controller_power) {
		data->fw_attrs_dev = device_create(&firmware_attributes_class, NULL,
						MKDEV(0, 0), NULL, "%s",
						DRIVER_NAME);
		if (IS_ERR(data->fw_attrs_dev))
			return PTR_ERR(data->fw_attrs_dev);

		ret = devm_add_action_or_reset(&data->pdev->dev,
					ayaneo_fw_attrs_dev_unregister,
					data->fw_attrs_dev);
		if (ret)
			return ret;

		data->fw_attrs_kset = kset_create_and_add("attributes", NULL,
							&data->fw_attrs_dev->kobj);
		if (!data->fw_attrs_kset)
			return -ENOMEM;

		ret = devm_add_action_or_reset(&data->pdev->dev, ayaneo_kset_unregister,
					data->fw_attrs_kset);

		if (data->quirks->has_magic_modules) {
			ret = ayaneo_fw_attr_init(
				data, AYANEO_ATTR_CONTROLLER_MODULES, true);
			if (ret)
				return ret;
		}

		if (data->quirks->has_controller_power) {
			ret = ayaneo_fw_attr_init(
				data, AYANEO_ATTR_CONTROLLER_POWER, false);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static struct platform_driver ayaneo_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
	},
	.probe = ayaneo_ec_probe,
};

static struct platform_device *ayaneo_platform_device;

static int __init ayaneo_ec_init(void)
{
	ayaneo_platform_device =
		platform_create_bundle(&ayaneo_platform_driver,
				       ayaneo_ec_probe, NULL, 0, NULL, 0);

	return PTR_ERR_OR_ZERO(ayaneo_platform_device);
}

static void __exit ayaneo_ec_exit(void)
{
	platform_device_unregister(ayaneo_platform_device);
	platform_driver_unregister(&ayaneo_platform_driver);
}

MODULE_DEVICE_TABLE(dmi, dmi_table);

module_init(ayaneo_ec_init);
module_exit(ayaneo_ec_exit);

MODULE_AUTHOR("Antheas Kapenekakis <lkml@antheas.dev>");
MODULE_DESCRIPTION("Ayaneo Embedded Controller (EC) platform features");
MODULE_LICENSE("GPL");
