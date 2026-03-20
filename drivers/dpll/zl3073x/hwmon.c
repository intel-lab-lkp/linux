// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/hwmon.h>

#include "core.h"
#include "hwmon.h"
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

int zl3073x_hwmon_init(struct zl3073x_dev *zldev)
{
	struct device *hwmon;

	hwmon = devm_hwmon_device_register_with_info(zldev->dev, "zl3073x",
						     zldev,
						     &zl3073x_hwmon_chip_info,
						     NULL);
	return PTR_ERR_OR_ZERO(hwmon);
}
