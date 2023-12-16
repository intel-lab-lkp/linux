// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for AMS AS6200 Temperature sensor
 *
 * Author: Abdel Alkuor <alkuor@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <linux/hwmon.h>

#define AS6200_TVAL_REG		0x0
#define AS6200_CONFIG_REG	0x1
#define AS6200_TLOW_REG		0x2
#define AS6200_THIGH_REG	0x3

#define AS6200_CONFIG_AL	BIT(5)
#define AS6200_CONFIG_CR	GENMASK(7, 6)
#define AS6200_CONFIG_SM	BIT(8)
#define AS6200_CONFIG_IM	BIT(9)
#define AS6200_CONFIG_POL	BIT(10)
#define AS6200_CONFIG_CF	GENMASK(12, 11)

#define AS6200_TEMP_MASK	GENMASK(15, 4)
#define AS6200_DEFAULT_CONFIG	(AS6200_CONFIG_CR |\
				 AS6200_CONFIG_CF |\
				 AS6200_CONFIG_POL)

static const struct regmap_config as6200_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AS6200_THIGH_REG,
};

static irqreturn_t as6200_event_handler(int irq, void *private)
{
	struct device *hwmon_dev = private;

	hwmon_notify_event(hwmon_dev, hwmon_temp, hwmon_temp_alarm, 0);
	return IRQ_HANDLED;
}

static int as6200_read(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *val)
{
	struct regmap *regmap = dev_get_drvdata(dev);
	unsigned int regval;
	unsigned int reg;
	s32 temp;
	int ret;

	switch (attr) {
	case hwmon_temp_input:
		reg = AS6200_TVAL_REG;
		break;
	case hwmon_temp_max_hyst:
		reg = AS6200_TLOW_REG;
		break;
	case hwmon_temp_max:
		reg = AS6200_THIGH_REG;
		break;
	case hwmon_temp_alarm:
		reg = AS6200_CONFIG_REG;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = regmap_read(regmap, reg, &regval);
	if (ret)
		return ret;

	if (reg == AS6200_CONFIG_REG) {
		*val = FIELD_GET(AS6200_CONFIG_AL, regval);
	} else {
		temp = sign_extend32(FIELD_GET(AS6200_TEMP_MASK, regval), 11);
		*val = DIV_ROUND_CLOSEST(temp * 625, 10);
	}

	return 0;
}

static int as6200_write(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long val)
{
	struct regmap *regmap = dev_get_drvdata(dev);
	int reg;

	switch (attr) {
	case hwmon_temp_max_hyst:
		reg = AS6200_TLOW_REG;
		break;
	case hwmon_temp_max:
		reg = AS6200_THIGH_REG;
		break;
	default:
		return -EOPNOTSUPP;
	}

	val = clamp_val(val, -40000, 125000) * 16 / 1000;
	return regmap_write(regmap, reg, FIELD_PREP(AS6200_TEMP_MASK, val));
}

static umode_t as6200_is_visible(const void *data, enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	if (type != hwmon_temp)
		return 0;

	switch (attr) {
	case hwmon_temp_input:
	case hwmon_temp_alarm:
		return 0444;
	case hwmon_temp_max_hyst:
	case hwmon_temp_max:
		return 0644;
	default:
		return 0;
	}
}

static const struct hwmon_ops as6200_hwmon_ops = {
	.is_visible = as6200_is_visible,
	.read = as6200_read,
	.write = as6200_write,
};

static const struct hwmon_channel_info * const as6200_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MAX |
			   HWMON_T_MAX_HYST | HWMON_T_ALARM),
	NULL
};

struct hwmon_chip_info as6200_chip_info = {
	.ops = &as6200_hwmon_ops,
	.info = as6200_info
};

static int as6200_probe(struct i2c_client *client)
{
	struct regmap *regmap;
	struct device *hwmon_dev;
	struct device *dev = &client->dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EINVAL;

	regmap = devm_regmap_init_i2c(client, &as6200_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret,
				     "Could not get and enable regulator %d\n",
				     ret);

	ret = regmap_write(regmap, AS6200_CONFIG_REG, AS6200_DEFAULT_CONFIG);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "as6200",
							 regmap,
							 &as6200_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	if (client->irq) {
		ret = devm_request_threaded_irq(dev,
						client->irq,
						NULL,
						&as6200_event_handler,
						IRQF_ONESHOT,
						client->name,
						hwmon_dev);
		if (ret)
			return ret;
	}

	i2c_set_clientdata(client, regmap);

	return 0;
}

static int __maybe_unused as6200_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct regmap *regmap = i2c_get_clientdata(client);

	if (client->irq)
		disable_irq(client->irq);

	return regmap_update_bits(regmap, AS6200_CONFIG_REG,
				  AS6200_CONFIG_SM, AS6200_CONFIG_SM);
}

static int __maybe_unused as6200_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct regmap *regmap = i2c_get_clientdata(client);
	int ret;

	ret = regmap_update_bits(regmap, AS6200_CONFIG_REG, AS6200_CONFIG_SM, 0);
	if (ret)
		return ret;

	if (client->irq)
		enable_irq(client->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(as6200_pm_ops, as6200_suspend, as6200_resume);

static const struct i2c_device_id as6200_id_table[] = {
	{ "as6200", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, as6200_id_table);

static const struct of_device_id as6200_of_match[] = {
	{ .compatible = "ams,as6200" },
	{ }
};
MODULE_DEVICE_TABLE(of, as6200_of_match);

static struct i2c_driver as6200_driver = {
	.driver = {
		.name = "as6200",
		.pm = pm_sleep_ptr(&as6200_pm_ops),
		.of_match_table = as6200_of_match,
	},
	.probe = as6200_probe,
	.id_table = as6200_id_table,
};
module_i2c_driver(as6200_driver);

MODULE_AUTHOR("Abdel Alkuor <alkuor@gmail.com");
MODULE_DESCRIPTION("AMS AS6200 Temperature Sensor");
MODULE_LICENSE("GPL");
