// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for sensors and fan control on Siemens SIMATIC IPC embedded controllers.
 *
 * Due to historical reasons the ACPI description for the ec doesn't
 * follow the standard ACPI descriptions for fans and hwmon sensors (using
 * the generic drviers/acpi/ec.c driver).
 *
 * This driver binds to Siemens SIMATIC IPC embedded controller sensor devices.
 *
 * Copyright (c) Siemens AG, 2025-2026
 *
 * Authors:
 *  Gao Nian <nian.gao@siemens.com>
 *  Benedikt Niedermayr <benedikt.niedermayr@siemens.com>
 */
#include <linux/acpi.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

/*
 * The ACPI ID domain "110A" belongs to Siemens
 * https://admin.pci-ids.ucw.cz/read/PC/110a
 */
#define SIEMENS_FAN_HID		"110A5002"
#define SIEMENS_HWMON_HID	"110A5004"

#define CHIP_NAME_MAX_LEN	64

/*
 * All temperature sensors are located in an ACPI device with SIEMENS_HWMON_HID.
 */
#define TEMP_REG_CTR_STATUS		0	/* u8 */
#define TEMP_REG_VALUE			1	/* char */
#define TEMP_REG_ALARM_LOWLIMIT		2	/* char */
#define TEMP_REG_ALARM_HIGHLIMIT	3	/* char */
#define TEMP_REG_CRITICAL		4	/* char */

#define SIZE_OF_TEMP_REG		5

#define CRITICAL_SUPPORT		BIT(5)
#define ALARM_SUPPORT			BIT(4)

/*
 * Each fan device is a separate ACPI device with the same SIEMENS_FAN_HID.
 */
#define FAN_REG_CTR_STATUS		0	/* u8 */
#define FAN_REG_SPEED			1	/* u16 */
#define FAN_REG_MODE			3	/* u8 */
#define FAN_REG_DUTY_LEVEL		4	/* u8 */

/*
 * CONTROL MODE:
 * 0: Full speed
 * 1: Manual fan speed control enabled
 * 2: Automatic fan speed control enabled
 */
#define CONTROL_MODE_MASK		(BIT(1) | BIT(0))
#define CONTROL_MODE_GET(mode)		((mode) & CONTROL_MODE_MASK)

struct simatic_ipc_ec_temp {
	u8 base;
	u8 count;
};

struct simatic_ipc_ec_fan {
	u8 base;
	const char *label;
	char chip_name[CHIP_NAME_MAX_LEN];
};

struct simatic_ipc_ec_sensor {
	struct simatic_ipc_ec_temp *temp;	/* Optional */
	struct simatic_ipc_ec_fan *fan;	/* Optional */
};

static inline u8 temp_reg(u8 base, u8 ch, u8 offset)
{
	return base + ch * SIZE_OF_TEMP_REG + offset;
}

static inline u8 fan_reg(u8 base, u8 offset)
{
	return base + offset;
}

static umode_t simatic_ipc_ec_temp_is_visible(const struct simatic_ipc_ec_temp *temp,
					      u32 attr, int channel)
{
	switch (attr) {
	case hwmon_temp_input:
		return 0444;
	case hwmon_temp_max_alarm:
	case hwmon_temp_min_alarm:
	case hwmon_temp_crit:
		return 0644;
	default:
		break;
	}

	return 0;
}

static int simatic_ipc_ec_temp_read(const struct simatic_ipc_ec_temp *temp, u32 attr,
				    int channel, long *val)
{
	s8 value;
	int ret;
	u8 reg;

	switch (attr) {
	case hwmon_temp_input:
		reg = temp_reg(temp->base, channel, TEMP_REG_VALUE);
		break;
	case hwmon_temp_max_alarm:
		reg = temp_reg(temp->base, channel, TEMP_REG_ALARM_HIGHLIMIT);
		break;
	case hwmon_temp_min_alarm:
		reg = temp_reg(temp->base, channel, TEMP_REG_ALARM_LOWLIMIT);
		break;
	case hwmon_temp_crit:
		reg = temp_reg(temp->base, channel, TEMP_REG_CRITICAL);
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = ec_read(reg, &value);

	/* Degree Celsius to millidegree Celsius */
	*val = value * 1000;

	return ret;
}

static int simatic_ipc_ec_temp_write(const struct simatic_ipc_ec_temp *temp, u32 attr,
				     int channel, long val)
{
	/*
	 * Millidegree Celsius to degree Celsius
	 */
	s8 value = (s8)clamp((s8)DIV_ROUND_CLOSEST(val, 1000), (s8)-128, (s8)127);
	u8 reg;

	switch (attr) {
	case hwmon_temp_max_alarm:
		reg = temp_reg(temp->base, channel, TEMP_REG_ALARM_HIGHLIMIT);
		break;
	case hwmon_temp_min_alarm:
		reg = temp_reg(temp->base, channel, TEMP_REG_ALARM_LOWLIMIT);
		break;
	case hwmon_temp_crit:
		reg = temp_reg(temp->base, channel, TEMP_REG_CRITICAL);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return ec_write(reg, value);
}

static umode_t simatic_ipc_ec_fan_is_visible(const struct simatic_ipc_ec_fan *fan,
					     enum hwmon_sensor_types type, u32 attr,
					     int channel)
{
	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

static int simatic_ipc_ec_read_block(u8 addr, void *buf, u8 n)
{
	int ret;
	u8 i;

	for (i = 0; i < n; i++) {
		ret = ec_read(addr + i, &((u8 *)buf)[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int simatic_ipc_ec_fan_read(struct simatic_ipc_ec_fan *fan, enum hwmon_sensor_types type,
				   u32 attr, int channel, long *val)
{
	u16 value_u16;
	u8 value_u8;
	int ret;
	u8 reg;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			reg = fan_reg(fan->base, FAN_REG_SPEED);
			ret = simatic_ipc_ec_read_block(reg, &value_u16, sizeof(value_u16));
			*val = value_u16;
			return ret;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			reg = fan_reg(fan->base, FAN_REG_DUTY_LEVEL);
			ret = ec_read(reg, &value_u8);
			*val = value_u8;
			return ret;
		case hwmon_pwm_enable:
			reg = fan_reg(fan->base, FAN_REG_MODE);
			ret = ec_read(reg, &value_u8);
			*val = CONTROL_MODE_GET(value_u8);
			return ret;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int simatic_ipc_ec_fan_read_string(struct simatic_ipc_ec_fan *fan, u32 attr,
					 const char **str)
{
	switch (attr) {
	case hwmon_fan_label:
		*str = fan->label;
		return 0;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int simatic_ipc_ec_fan_set_control_mode(struct simatic_ipc_ec_fan *fan, u8 ctrl_mode)
{
	int ret;
	u8 mode;

	/* 0: full speed, 1: mannual control, 2: auto-control */
	if (ctrl_mode > 2)
		return -EINVAL;

	ret = ec_read(fan_reg(fan->base, FAN_REG_MODE), &mode);
	if (ret < 0)
		return ret;

	mode &= ~CONTROL_MODE_MASK;
	mode |= ctrl_mode;

	ret = ec_write(fan_reg(fan->base, FAN_REG_MODE), mode);
	if (ret < 0)
		return ret;

	return 0;
}

static int simatic_ipc_ec_fan_write(struct simatic_ipc_ec_fan *fan, u32 attr, int channel,
				    long val)
{
	switch (attr) {
	case hwmon_pwm_input:
		if (val < 0 || val > 0xFF)
			return -EINVAL;
		return ec_write(fan_reg(fan->base, FAN_REG_DUTY_LEVEL), (u8)val);
	case hwmon_pwm_enable:
		return simatic_ipc_ec_fan_set_control_mode(fan, (u8)val);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static umode_t simatic_ipc_ec_is_visible(const void *drvdata, enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	const struct simatic_ipc_ec_sensor *data = drvdata;
	umode_t ret = 0;

	switch (type) {
	case hwmon_temp:
		if (data->temp)
			ret = simatic_ipc_ec_temp_is_visible(data->temp, attr, channel);
		break;
	case hwmon_fan:
	case hwmon_pwm:
		if (data->fan)
			ret = simatic_ipc_ec_fan_is_visible(data->fan, type, attr, channel);
		break;
	default:
		break;
	}

	return ret;
}

static int simatic_ipc_ec_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	struct simatic_ipc_ec_sensor *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		return simatic_ipc_ec_temp_read(data->temp, attr, channel, val);
	case hwmon_fan:
	case hwmon_pwm:
		return simatic_ipc_ec_fan_read(data->fan, type, attr, channel, val);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int simatic_ipc_ec_read_string(struct device *dev, enum hwmon_sensor_types type,
				     u32 attr, int channel, const char **str)
{
	struct simatic_ipc_ec_sensor *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_fan:
	case hwmon_pwm:
		return simatic_ipc_ec_fan_read_string(data->fan, attr, str);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int simatic_ipc_ec_write(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long val)
{
	struct simatic_ipc_ec_sensor *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		return simatic_ipc_ec_temp_write(data->temp, attr, channel, val);
	case hwmon_fan:
	case hwmon_pwm:
		return simatic_ipc_ec_fan_write(data->fan, attr, channel, val);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_ops simatic_ipc_ec_ops = {
	.is_visible = simatic_ipc_ec_is_visible,
	.read = simatic_ipc_ec_read,
	.read_string = simatic_ipc_ec_read_string,
	.write = simatic_ipc_ec_write,
};

/*
 * The number of temperature sensor's channels comes from ACPI as it may be different for
 * different projects.
 */
static struct hwmon_channel_info simatic_ipc_ec_temp_ch_info = {
	.type = hwmon_temp,
	.config = NULL
};

static const struct hwmon_channel_info *simatic_ipc_ec_hwmon_ch_info[] = {
	&simatic_ipc_ec_temp_ch_info,
	NULL
};

static const struct hwmon_channel_info *simatic_ipc_ec_fan_ch_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info simatic_ipc_ec_hwmon_info = {
	.ops = &simatic_ipc_ec_ops,
	.info = simatic_ipc_ec_hwmon_ch_info,
};

static const struct hwmon_chip_info simatic_ipc_ec_fan_info = {
	.ops = &simatic_ipc_ec_ops,
	.info = simatic_ipc_ec_fan_ch_info,
};

static int simatic_ipc_ec_hwmon_probe(struct device *dev, struct simatic_ipc_ec_sensor *data)
{
	struct simatic_ipc_ec_temp *temp;
	struct device *hwmon_dev;
	u32 *temp_config;
	u8 temp_count;
	u8 temp_base;
	u8 value;
	int ret;
	u8 ch;

	ret = device_property_read_u8(dev, "TempRegOffset", &temp_base);
	if (ret < 0)
		return ret;

	ret = device_property_read_u8(dev, "TempCount", &temp_count);
	if (ret < 0)
		return ret;

	temp = devm_kzalloc(dev, sizeof(*temp), GFP_KERNEL);
	if (!temp)
		return -ENOMEM;

	temp_config = devm_kzalloc(dev, (temp_count + 1) * sizeof(u32), GFP_KERNEL);
	if (!temp_config)
		return -ENOMEM;

	simatic_ipc_ec_temp_ch_info.config = temp_config;

	temp->base = temp_base;
	temp->count = temp_count;

	data->temp = temp;

	/* Dynamically create temperature sensor channels */
	for (ch = 0; ch < temp_count; ch++) {
		ret = ec_read(temp_reg(temp_base, ch, TEMP_REG_CTR_STATUS), &value);
		if (ret < 0)
			return ret;

		temp_config[ch] |= HWMON_T_INPUT;

		if (value & ALARM_SUPPORT)
			temp_config[ch] |= HWMON_T_MIN_ALARM | HWMON_T_MAX_ALARM;

		if (value & CRITICAL_SUPPORT)
			temp_config[ch] |= HWMON_T_CRIT;
	}

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "simatic_ipc_ec_hwmon",
							 data, &simatic_ipc_ec_hwmon_info,
							 NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static int simatic_ipc_ec_fan_probe(struct device *dev, struct simatic_ipc_ec_sensor *data)
{
	struct device *hwmon_dev;
	struct simatic_ipc_ec_fan *fan;
	const char *label, *fan_name = NULL;
	u8 fan_base;
	int ret;

	ret = device_property_read_u8(dev, "RegOffset", &fan_base);
	if (ret < 0)
		return ret;

	ret = device_property_read_string(dev, "Label", &label);
	if (ret < 0)
		return ret;

	device_property_read_string(dev, "FanName", &fan_name);

	fan = devm_kzalloc(dev, sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;

	fan->base = fan_base;
	fan->label = fan_name ? fan_name : label;

	if (fan_name) {
		char *p;

		snprintf(fan->chip_name, CHIP_NAME_MAX_LEN, "simatic_ipc_ec_%s", fan_name);
		for (p = fan->chip_name; *p; p++)
			*p = tolower(*p);
	} else {
		snprintf(fan->chip_name, CHIP_NAME_MAX_LEN, "simatic_ipc_ec_%s", label);
	}

	data->fan = fan;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, fan->chip_name, data,
							 &simatic_ipc_ec_fan_info,
							 NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static int simatic_ipc_ec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct simatic_ipc_ec_sensor *data;
	const char *hid;
	int ret;

	/* The same HID sensor device may not be on the EC. */
	ret = device_property_match_string(dev, "ChipType", "ec");
	if (ret < 0)
		return -ENODEV;

	hid = acpi_device_hid(to_acpi_device_node(dev->fwnode));

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (!strcmp(hid, SIEMENS_HWMON_HID))
		ret = simatic_ipc_ec_hwmon_probe(dev, data);
	else if (!strcmp(hid, SIEMENS_FAN_HID))
		ret = simatic_ipc_ec_fan_probe(dev, data);
	else
		ret = -EINVAL;

	return ret;
}

static const struct acpi_device_id simatic_ipc_ec_sensors_ids[] = {
	{SIEMENS_HWMON_HID},
	{SIEMENS_FAN_HID},
	{}
};

MODULE_DEVICE_TABLE(acpi, simatic_ipc_ec_sensors_ids);

static struct platform_driver simatic_ipc_ec_sensors_driver = {
	.probe = simatic_ipc_ec_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.acpi_match_table = simatic_ipc_ec_sensors_ids
	},
};

module_platform_driver(simatic_ipc_ec_sensors_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gao Nian <nian.gao@siemens.com>");
MODULE_DESCRIPTION("Siemens SIMATIC IPC embedded controller sensors driver");
