// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>

#include "core.h"
#include "hwmon.h"
#include "ref.h"
#include "regs.h"

static int zl3073x_hwmon_read(struct device *dev,
			      enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct zl3073x_dev *zldev = dev_get_drvdata(dev);
	u16 raw;
	int rc;

	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;

	rc = zl3073x_read_u16(zldev, ZL_REG_DIE_TEMP_STATUS, &raw);
	if (rc)
		return rc;

	/* Convert from 0.1°C units to millidegrees Celsius */
	*val = (s16)raw * 100;

	return 0;
}

static umode_t zl3073x_hwmon_is_visible(const void *data,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	const struct zl3073x_dev *zldev = data;

	if (type == hwmon_temp && (zldev->info->flags & ZL3073X_FLAG_DIE_TEMP))
		return 0444;

	return 0;
}

static const struct hwmon_channel_info * const zl3073x_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL,
};

static const struct hwmon_ops zl3073x_hwmon_ops = {
	.is_visible = zl3073x_hwmon_is_visible,
	.read = zl3073x_hwmon_read,
};

static const struct hwmon_chip_info zl3073x_hwmon_chip_info = {
	.ops = &zl3073x_hwmon_ops,
	.info = zl3073x_hwmon_info,
};

static ssize_t freq_input_show(struct device *dev,
			       struct device_attribute *devattr, char *buf)
{
	struct zl3073x_dev *zldev = dev_get_drvdata(dev);
	int index = to_sensor_dev_attr(devattr)->index;
	const struct zl3073x_ref *ref;

	if (!zldev->ready)
		return -ENODATA;

	ref = zl3073x_ref_state_get(zldev, index);

	return sysfs_emit(buf, "%u\n", zl3073x_ref_meas_freq_get(ref));
}

static ssize_t freq_label_show(struct device *dev,
			       struct device_attribute *devattr, char *buf)
{
	static const char * const labels[] = {
		"REF0P", "REF0N", "REF1P", "REF1N", "REF2P",
		"REF2N", "REF3P", "REF3N", "REF4P", "REF4N",
	};
	int index = to_sensor_dev_attr(devattr)->index;

	return sysfs_emit(buf, "%s\n", labels[index]);
}

static SENSOR_DEVICE_ATTR_RO(freq0_input, freq_input, 0);
static SENSOR_DEVICE_ATTR_RO(freq1_input, freq_input, 1);
static SENSOR_DEVICE_ATTR_RO(freq2_input, freq_input, 2);
static SENSOR_DEVICE_ATTR_RO(freq3_input, freq_input, 3);
static SENSOR_DEVICE_ATTR_RO(freq4_input, freq_input, 4);
static SENSOR_DEVICE_ATTR_RO(freq5_input, freq_input, 5);
static SENSOR_DEVICE_ATTR_RO(freq6_input, freq_input, 6);
static SENSOR_DEVICE_ATTR_RO(freq7_input, freq_input, 7);
static SENSOR_DEVICE_ATTR_RO(freq8_input, freq_input, 8);
static SENSOR_DEVICE_ATTR_RO(freq9_input, freq_input, 9);

static SENSOR_DEVICE_ATTR_RO(freq0_label, freq_label, 0);
static SENSOR_DEVICE_ATTR_RO(freq1_label, freq_label, 1);
static SENSOR_DEVICE_ATTR_RO(freq2_label, freq_label, 2);
static SENSOR_DEVICE_ATTR_RO(freq3_label, freq_label, 3);
static SENSOR_DEVICE_ATTR_RO(freq4_label, freq_label, 4);
static SENSOR_DEVICE_ATTR_RO(freq5_label, freq_label, 5);
static SENSOR_DEVICE_ATTR_RO(freq6_label, freq_label, 6);
static SENSOR_DEVICE_ATTR_RO(freq7_label, freq_label, 7);
static SENSOR_DEVICE_ATTR_RO(freq8_label, freq_label, 8);
static SENSOR_DEVICE_ATTR_RO(freq9_label, freq_label, 9);

static struct attribute *zl3073x_freq_attrs[] = {
	&sensor_dev_attr_freq0_input.dev_attr.attr,
	&sensor_dev_attr_freq0_label.dev_attr.attr,
	&sensor_dev_attr_freq1_input.dev_attr.attr,
	&sensor_dev_attr_freq1_label.dev_attr.attr,
	&sensor_dev_attr_freq2_input.dev_attr.attr,
	&sensor_dev_attr_freq2_label.dev_attr.attr,
	&sensor_dev_attr_freq3_input.dev_attr.attr,
	&sensor_dev_attr_freq3_label.dev_attr.attr,
	&sensor_dev_attr_freq4_input.dev_attr.attr,
	&sensor_dev_attr_freq4_label.dev_attr.attr,
	&sensor_dev_attr_freq5_input.dev_attr.attr,
	&sensor_dev_attr_freq5_label.dev_attr.attr,
	&sensor_dev_attr_freq6_input.dev_attr.attr,
	&sensor_dev_attr_freq6_label.dev_attr.attr,
	&sensor_dev_attr_freq7_input.dev_attr.attr,
	&sensor_dev_attr_freq7_label.dev_attr.attr,
	&sensor_dev_attr_freq8_input.dev_attr.attr,
	&sensor_dev_attr_freq8_label.dev_attr.attr,
	&sensor_dev_attr_freq9_input.dev_attr.attr,
	&sensor_dev_attr_freq9_label.dev_attr.attr,
	NULL,
};

static const struct attribute_group zl3073x_freq_group = {
	.attrs = zl3073x_freq_attrs,
};

static const struct attribute_group *zl3073x_hwmon_groups[] = {
	&zl3073x_freq_group,
	NULL,
};

int zl3073x_hwmon_init(struct zl3073x_dev *zldev)
{
	struct device *hwmon;

	hwmon = devm_hwmon_device_register_with_info(zldev->dev, "zl3073x",
						     zldev,
						     &zl3073x_hwmon_chip_info,
						     zl3073x_hwmon_groups);
	return PTR_ERR_OR_ZERO(hwmon);
}
