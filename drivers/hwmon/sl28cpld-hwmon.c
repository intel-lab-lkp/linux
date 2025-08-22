// SPDX-License-Identifier: GPL-2.0-only
/*
 * sl28cpld hardware monitoring driver
 *
 * Copyright 2020 Kontron Europe GmbH
 */

#include <linux/bitfield.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define FAN_INPUT		0x00
#define   FAN_SCALE_X8		BIT(7)
#define   FAN_VALUE_MASK	GENMASK(6, 0)

#define SA67MCU_VOLTAGE(n)	(0x00 + ((n) * 2))
#define SA67MCU_TEMP(n)		(0x04 + ((n) * 2))

struct sl28cpld_hwmon {
	struct regmap *regmap;
	u32 offset;
};

static int sl28cpld_hwmon_read(struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       int channel, long *input)
{
	struct sl28cpld_hwmon *hwmon = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	switch (attr) {
	case hwmon_fan_input:
		ret = regmap_read(hwmon->regmap, hwmon->offset + FAN_INPUT,
				  &value);
		if (ret)
			return ret;
		/*
		 * The register has a 7 bit value and 1 bit which indicates the
		 * scale. If the MSB is set, then the lower 7 bit has to be
		 * multiplied by 8, to get the correct reading.
		 */
		if (value & FAN_SCALE_X8)
			value = FIELD_GET(FAN_VALUE_MASK, value) << 3;

		/*
		 * The counter period is 1000ms and the sysfs specification
		 * says we should assume 2 pulses per revolution.
		 */
		value *= 60 / 2;

		break;
	default:
		return -EOPNOTSUPP;
	}

	*input = value;
	return 0;
}

static const struct hwmon_channel_info * const sl28cpld_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	NULL
};

static const struct hwmon_ops sl28cpld_hwmon_ops = {
	.visible = 0444,
	.read = sl28cpld_hwmon_read,
};

static const struct hwmon_chip_info sl28cpld_hwmon_chip_info = {
	.ops = &sl28cpld_hwmon_ops,
	.info = sl28cpld_hwmon_info,
};

static int sa67mcu_hwmon_read(struct device *dev,
			      enum hwmon_sensor_types type, u32 attr,
			      int channel, long *input)
{
	struct sl28cpld_hwmon *hwmon = dev_get_drvdata(dev);
	unsigned int offset;
	u8 reg[2];
	int ret;

	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			offset = hwmon->offset + SA67MCU_VOLTAGE(channel);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			offset = hwmon->offset + SA67MCU_TEMP(channel);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* Reading the low byte will capture the value */
	ret = regmap_bulk_read(hwmon->regmap, offset, reg, ARRAY_SIZE(reg));
	if (ret)
		return ret;

	*input = reg[1] << 8 | reg[0];

	/* Temperatures are s16 and in 0.1degC steps. */
	if (type == hwmon_temp)
		*input = sign_extend32(*input, 15) * 100;

	return 0;
}

static const struct hwmon_channel_info * const sa67mcu_hwmon_info[] = {
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT, HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_ops sa67mcu_hwmon_ops = {
	.visible = 0444,
	.read = sa67mcu_hwmon_read,
};

static const struct hwmon_chip_info sa67mcu_hwmon_chip_info = {
	.ops = &sa67mcu_hwmon_ops,
	.info = sa67mcu_hwmon_info,
};

static int sl28cpld_hwmon_probe(struct platform_device *pdev)
{
	const struct hwmon_chip_info *chip_info;
	struct sl28cpld_hwmon *hwmon;
	struct device *hwmon_dev;
	int ret;

	if (!pdev->dev.parent)
		return -ENODEV;

	chip_info = device_get_match_data(&pdev->dev);
	if (!chip_info)
		return -EINVAL;

	hwmon = devm_kzalloc(&pdev->dev, sizeof(*hwmon), GFP_KERNEL);
	if (!hwmon)
		return -ENOMEM;

	hwmon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!hwmon->regmap)
		return -ENODEV;

	ret = device_property_read_u32(&pdev->dev, "reg", &hwmon->offset);
	if (ret)
		return -EINVAL;

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
				"sl28cpld_hwmon", hwmon, chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		dev_err(&pdev->dev, "failed to register as hwmon device");

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id sl28cpld_hwmon_of_match[] = {
	{ .compatible = "kontron,sl28cpld-fan", .data = &sl28cpld_hwmon_chip_info },
	{ .compatible = "kontron,sa67mcu-hwmon", .data = &sa67mcu_hwmon_chip_info },
	{}
};
MODULE_DEVICE_TABLE(of, sl28cpld_hwmon_of_match);

static struct platform_driver sl28cpld_hwmon_driver = {
	.probe = sl28cpld_hwmon_probe,
	.driver = {
		.name = "sl28cpld-fan",
		.of_match_table = sl28cpld_hwmon_of_match,
	},
};
module_platform_driver(sl28cpld_hwmon_driver);

MODULE_DESCRIPTION("sl28cpld Hardware Monitoring Driver");
MODULE_AUTHOR("Michael Walle <michael@walle.cc>");
MODULE_LICENSE("GPL");
