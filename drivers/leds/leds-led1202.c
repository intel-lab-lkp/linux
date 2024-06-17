// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple LED driver for ST LED1202 chip
 *
 * Copyright (C) 2024 Remote-Tech Ltd. UK
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#define DRIVER_NAME "led-driver-1202"
#define DRIVER_VERSION "0.0.1"

#define LL1202_MAX_LEDS 12

#define LL1202_DEVICE_ID 0x00
#define LL1202_DEV_ENABLE 0x01
#define LL1202_CHAN_ENABLE_LOW 0x02
#define LL1202_CHAN_ENABLE_HIGH 0x03
#define LL1202_CONFIG_REG 0x04
#define LL1202_ILED_REG0 0x09
#define LL1202_PATTERN_REP 0x15
#define LL1202_PATTERN_DUR 0x16
#define LL1202_PATTERN_PWM 0x1E
#define LL1202_CLOCK_REG 0xE0

struct ll1202_led {
	struct led_classdev led_cdev;
	struct ll1202_chip *chip;
	int led_num;
	char name[32];
	int is_active;
};

struct ll1202_chip {
	struct i2c_client *client;
	struct mutex lock;
	struct ll1202_led leds[LL1202_MAX_LEDS];
};

static struct ll1202_led *cdev_to_ll1202_led(struct led_classdev *cdev)
{
	return container_of(cdev, struct ll1202_led, led_cdev);
}

static int ll1202_read_reg(struct ll1202_chip *chip, int reg, uint8_t *val)
{
	int ret = i2c_smbus_read_byte_data(chip->client, reg);

	if (ret < 0)
		return ret;

	*val = (uint8_t)ret;
	return 0;
}

static int ll1202_write_reg(struct ll1202_chip *chip, int reg, uint8_t val)
{
	return i2c_smbus_write_byte_data(chip->client, reg, val);
}

static int ll1202_get_channel(struct device *dev)
{
	struct device_node *np = dev->parent->of_node, *child;
	int err, ret = -1;

	for_each_child_of_node(np, child) {
		if (strncmp(dev->kobj.name,
			    of_get_property(child, "label", NULL),
			    strnlen(dev->kobj.name, MAX_INPUT)) == 0) {
			err = of_property_read_u32(child, "reg", &ret);
			if (err) {
				of_node_put(child);
				pr_err(DRIVER_NAME
				       ": Failed to read property %s", child->name);
				return ret;
			}
			break;
		}
	}
	return ret;
}

static ssize_t ll1202_show_all_registers(struct device *dev,
					 struct device_attribute *devattr,
					 char *buf)
{
	struct ll1202_chip *chip = dev_get_drvdata(dev);
	uint8_t reg_value = 0;
	int ret, i;
	char *bufp = buf;

	mutex_lock(&chip->lock);

	for (i = LL1202_DEVICE_ID; i <= LL1202_CLOCK_REG; i++) {
		ret = ll1202_read_reg(chip, i, &reg_value);
		if (ret != 0)
			dev_err(&chip->client->dev,
				"Reading register [0x%x] failed.\n", i);

		bufp += snprintf(bufp, PAGE_SIZE, "Addr[0x%x] = 0x%x\n", i,
				 reg_value);
	}

	mutex_unlock(&chip->lock);
	return strlen(buf);
}

static ssize_t
ll1202_show_patt_sequence_repetition(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct ll1202_chip *chip = dev_get_drvdata(dev);
	unsigned int ret;
	uint8_t reg_value;
	char *bufp = buf;

	mutex_lock(&chip->lock);
	ret = ll1202_read_reg(chip, LL1202_PATTERN_REP, &reg_value);
	if (ret != 0)
		dev_err(&chip->client->dev, "Reading register [0x%x] failed\n", LL1202_PATTERN_REP);
	mutex_unlock(&chip->lock);
	bufp += snprintf(bufp, PAGE_SIZE,
			 "Pattern sequence register, repetition value = %d (times)\n",
			 reg_value);
	return strlen(buf);
}

static ssize_t
ll1202_store_patt_sequence_repetition(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct ll1202_chip *chip = dev_get_drvdata(dev);
	unsigned int ret;
	unsigned long duration;

	if (!count)
		return -EINVAL;

	ret = kstrtoul(buf, 10, &duration);
	if (ret) {
		dev_err(&chip->client->dev, "sscanf failed with error :%d\n",
			ret);
		return ret;
	}

	mutex_lock(&chip->lock);
	ret = ll1202_write_reg(chip, LL1202_PATTERN_REP, duration);
	if (ret != 0)
		dev_err(&chip->client->dev, "Writing register [0x%x] failed\n",
			LL1202_PATTERN_REP);
	mutex_unlock(&chip->lock);
	return count;
}

static int ll1202_prescalar_to_miliamps(uint8_t reg_value)
{
	return reg_value * 20 / 255;
}

static int ll1202_prescalar_to_miliseconds(uint8_t reg_value)
{
	return reg_value * 5660 / 255;
}

static ssize_t ll1202_show_channel_mA_current(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct ll1202_chip *chip = dev_get_drvdata(dev->parent);
	unsigned int ret;
	uint8_t reg_value;
	char *bufp = buf;
	int led_num = ll1202_get_channel(dev);

	if (led_num < 0 || led_num >= LL1202_MAX_LEDS) {
		dev_err(&chip->client->dev,
			"Invalid register [0x%x] (out of range)\n",
			led_num);
	}
	mutex_lock(&chip->lock);
	ret = ll1202_read_reg(chip, LL1202_ILED_REG0 + led_num, &reg_value);
	if (ret != 0)
		dev_err(&chip->client->dev, "Reading analog dimming register [0x%x] failed\n",
			led_num);
	mutex_unlock(&chip->lock);
	bufp += snprintf(bufp, PAGE_SIZE, "Channel[%d] = %d mA\n", led_num,
			 ll1202_prescalar_to_miliamps(reg_value));
	return strlen(buf);
}

static int ll1202_channel_activate(struct ll1202_led *led)
{
	struct ll1202_chip *chip;
	uint8_t reg_chan_low, reg_chan_high;
	int ret = 0;

	chip = led->chip;
	if (led->is_active) {
		mutex_lock(&chip->lock);

		ret = ll1202_read_reg(chip, LL1202_CHAN_ENABLE_LOW,
				      &reg_chan_low);
		if (ret < 0) {
			dev_err(&chip->client->dev,
				"Failed reading register [0x%x]\n", LL1202_CHAN_ENABLE_LOW);
		}

		ret = ll1202_read_reg(chip, LL1202_CHAN_ENABLE_HIGH,
				      &reg_chan_high);
		if (ret < 0) {
			dev_err(&chip->client->dev,
				"Failed reading register [0x%x]\n", LL1202_CHAN_ENABLE_HIGH);
		}

		reg_chan_low = reg_chan_low | BIT(led->led_num);
		ret = ll1202_write_reg(chip, LL1202_CHAN_ENABLE_LOW,
				       reg_chan_low);
		if (ret < 0) {
			dev_err(&chip->client->dev,
				"Failed writing to register [0x%x]\n", LL1202_CHAN_ENABLE_LOW);
		}
		reg_chan_high = reg_chan_high | (BIT(led->led_num) >> 7);
		ret = ll1202_write_reg(chip, LL1202_CHAN_ENABLE_HIGH,
				       reg_chan_high);
		if (ret < 0) {
			dev_err(&chip->client->dev,
				"Failed writing to register [0x%x]\n", LL1202_CHAN_ENABLE_HIGH);
		}
		mutex_unlock(&chip->lock);
	}
	return ret;
}

#define LL1202_PWM_PATTERN_ATTR(pattern)                                          \
	static ssize_t ll1202_show_pwm_pattern##pattern(                          \
		struct device *dev, struct device_attribute *attr, char *buf)     \
	{                                                                         \
		struct ll1202_chip *chip = dev_get_drvdata(dev->parent);          \
		uint8_t duration = 0;                                             \
		uint8_t reg_value_l = 0;                                          \
		uint8_t reg_value_h = 0;                                          \
		uint16_t reg_value = 0;                                           \
		int ret;                                                          \
		char *bufp = buf;                                                 \
		int led_num = ll1202_get_channel(dev);                            \
		if (led_num < 0 || led_num >= LL1202_MAX_LEDS) {                  \
			dev_err(&chip->client->dev,                               \
				"Invalid register [0x%x] (out of range)\n",  \
				led_num);                                         \
		}                                                                 \
		mutex_lock(&chip->lock);                                          \
		ret = ll1202_read_reg(                                            \
			chip,                                                     \
			(LL1202_PATTERN_PWM + (led_num * 2) + 0x18 * pattern),    \
			&reg_value_l);                                            \
		if (ret != 0)                                                     \
			dev_err(&chip->client->dev,                               \
				"Reading pattern PWM register [0x%x] failed\n", led_num);     \
		ret = ll1202_read_reg(chip,                                       \
				      (LL1202_PATTERN_PWM + 0x1 +                 \
				       (led_num * 2) + 0x18 * pattern),           \
				      &reg_value_h);                              \
		if (ret != 0)                                                     \
			dev_err(&chip->client->dev,                               \
				"Reading pattern PWM register [0x%x] failed\n", led_num);     \
		reg_value = (uint16_t)reg_value_h << 8 | reg_value_l;             \
		ret = ll1202_read_reg(chip, (LL1202_PATTERN_DUR + pattern),       \
				      &duration);                                 \
		if (ret != 0)                                                     \
			dev_err(&chip->client->dev,                               \
				"Reading pattern durating register [0x%x] failed\n", led_num);     \
		bufp += snprintf(                                                 \
			bufp, PAGE_SIZE,                                          \
			"Pattern[%d][cs%d]: PWM = 0x%03X; DURATION = %d ms\n",    \
			pattern, led_num, reg_value,                              \
			ll1202_prescalar_to_miliseconds(duration));               \
		mutex_unlock(&chip->lock);                                        \
		return strlen(buf);                                               \
	}                                                                         \
	static ssize_t ll1202_store_pwm_pattern##pattern(                         \
		struct device *dev, struct device_attribute *attr,                \
		const char *buf, size_t count)                                    \
	{                                                                         \
		struct ll1202_chip *chip = dev_get_drvdata(dev->parent);          \
		unsigned int ret, reg_value;                                      \
		unsigned long duration;                                           \
		char buf_u8[16];                                                  \
		uint8_t reg_value_l = 0;                                          \
		uint8_t reg_value_h = 0;                                          \
		int led_num = ll1202_get_channel(dev);                            \
		if (led_num < 0 || led_num >= LL1202_MAX_LEDS) {                  \
			dev_err(&chip->client->dev,                               \
				"Invalid register [0x%x] (out of range)\n",  \
				led_num);                                         \
			return count;                                             \
		}                                                                 \
		if (!count)                                                       \
			return -EINVAL;                                           \
		ret = sscanf(buf, "%X %s", &reg_value, buf_u8);                   \
		if (ret == 0) {                                                   \
			dev_err(&chip->client->dev,                               \
				"sscanf failed with error :%d\n", ret);           \
			return ret;                                               \
		}                                                                 \
		ret = kstrtoul(buf_u8, 10, &duration);                            \
		if (ret)                                                          \
			return ret;                                               \
		reg_value_l = (uint8_t)reg_value;                                 \
		reg_value_h = (uint8_t)(reg_value >> 8);                          \
		mutex_lock(&chip->lock);                                          \
		ret = ll1202_write_reg(                                           \
			chip,                                                     \
			(LL1202_PATTERN_PWM + (led_num * 2) + 0x18 * pattern),    \
			(uint8_t)reg_value_l);                                    \
		if (ret != 0)                                                     \
			dev_err(&chip->client->dev,                               \
				"Writing to register [0x%x] failed, value %d\n",  \
				LL1202_PATTERN_PWM + (led_num * 2) +              \
					0x18 * pattern,                           \
				reg_value_l);                                     \
		ret = ll1202_write_reg(chip,                                      \
				       (LL1202_PATTERN_PWM + 0x1 +                \
					(led_num * 2) + 0x18 * pattern),          \
				       (uint8_t)reg_value_h);                     \
		if (ret != 0)                                                     \
			dev_err(&chip->client->dev,                               \
				"Writing to register [0x%x] failed, value %d\n",  \
				(LL1202_PATTERN_PWM + 0x1 + (led_num * 2) +       \
				 0x18 * pattern),                                 \
				reg_value_h);                                     \
		ret = ll1202_write_reg(chip, (LL1202_PATTERN_DUR + pattern),      \
				       (u8)duration);                             \
		if (ret != 0)                                                     \
			dev_err(&chip->client->dev,                               \
				"Writing to register [0x%x] failed, value %d\n", \
				(LL1202_PATTERN_DUR + pattern), (u8)duration);    \
		ret = ll1202_write_reg(chip, LL1202_CONFIG_REG,                   \
				       (0xC0 | pattern));                         \
		if (ret != 0) {                                                   \
			dev_err(&chip->client->dev,                               \
				"Failed writing to reg [0x%x]\n", LL1202_CONFIG_REG);     \
		}                                                                 \
		mutex_unlock(&chip->lock);                                        \
		ll1202_channel_activate(&chip->leds[led_num]);                    \
		return count;                                                     \
	}                                                                         \
	static struct device_attribute dev_attr_led_pwm_pattern##pattern = {		\
	.attr = {							\
	.name = __stringify(pwm_pattern##pattern),					\
	.mode =  0x00444 | 0x00200,						\
	},								\
	.show = ll1202_show_pwm_pattern##pattern,				\
	.store = ll1202_store_pwm_pattern##pattern,				\
}

LL1202_PWM_PATTERN_ATTR(0);
LL1202_PWM_PATTERN_ATTR(1);
LL1202_PWM_PATTERN_ATTR(2);
LL1202_PWM_PATTERN_ATTR(3);
LL1202_PWM_PATTERN_ATTR(4);
LL1202_PWM_PATTERN_ATTR(5);
LL1202_PWM_PATTERN_ATTR(6);
LL1202_PWM_PATTERN_ATTR(7);

static DEVICE_ATTR(led_device_regsdump, 0x00444, ll1202_show_all_registers,
		   NULL);
static DEVICE_ATTR(patt_sequence_repetition, 0x00444 | 0x00200,
		   ll1202_show_patt_sequence_repetition,
		   ll1202_store_patt_sequence_repetition);
static DEVICE_ATTR(current_mA, 0x00444, ll1202_show_channel_mA_current, NULL);

static struct attribute *led_attrs[] = {
	&dev_attr_led_device_regsdump.attr,
	&dev_attr_patt_sequence_repetition.attr,
	NULL,
};

static struct attribute *led_group_attrs[] = {
	&dev_attr_led_pwm_pattern0.attr, &dev_attr_led_pwm_pattern1.attr,
	&dev_attr_led_pwm_pattern2.attr, &dev_attr_led_pwm_pattern3.attr,
	&dev_attr_led_pwm_pattern4.attr, &dev_attr_led_pwm_pattern5.attr,
	&dev_attr_led_pwm_pattern6.attr, &dev_attr_led_pwm_pattern7.attr,
	&dev_attr_current_mA.attr,	 NULL,
};

static struct attribute_group attr_group = {
	.attrs = led_attrs,
};

static struct attribute_group attr_pat_group = {
	.attrs = led_group_attrs,
};

static const struct attribute_group *ll1202_groups[] = { &attr_pat_group,
							 NULL };

static void ll1202_brightness_set(struct led_classdev *led_cdev,
				  enum led_brightness value)
{
	struct ll1202_led *led = cdev_to_ll1202_led(led_cdev);
	struct ll1202_chip *chip = led->chip;
	int ret;

	mutex_lock(&chip->lock);
	ret = ll1202_write_reg(chip, LL1202_ILED_REG0 + led->led_num, value);
	if (ret != 0)
		dev_err(&chip->client->dev, "Reading register [0x%x] failed\n",
			LL1202_ILED_REG0 + led->led_num);
	mutex_unlock(&chip->lock);
}

static enum led_brightness ll1202_brightness_get(struct led_classdev *led_cdev)
{
	struct ll1202_led *led = cdev_to_ll1202_led(led_cdev);
	struct ll1202_chip *chip = led->chip;
	uint8_t reg_value;
	int ret;

	mutex_lock(&chip->lock);
	ret = ll1202_read_reg(chip, LL1202_ILED_REG0 + led->led_num,
			      &reg_value);
	if (ret != 0)
		dev_err(&chip->client->dev, "Reading register [0x%x] failed\n",
			LL1202_ILED_REG0 + led->led_num);

	mutex_unlock(&chip->lock);
	return reg_value;
}

static int ll1202_dt_init(struct ll1202_chip *chip)
{
	struct device_node *np = chip->client->dev.of_node, *child;
	struct ll1202_led *led;
	int err, reg;

	for_each_child_of_node(np, child) {
		err = of_property_read_u32(child, "reg", &reg);
		if (err) {
			of_node_put(child);
			pr_err(DRIVER_NAME ": Failed to get child node");
			return err;
		}
		if (reg < 0 || reg >= LL1202_MAX_LEDS) {
			of_node_put(child);
			pr_err(DRIVER_NAME ": Invalid register value [0x%x] (out of range)", reg);
			return -EINVAL;
		}

		led = &chip->leds[reg];
		led->led_cdev.name = of_get_property(child, "label", NULL) ?:
					     child->name;

		err = of_property_read_u32(child, "active", &led->is_active);
		if (err) {
			of_node_put(child);
			pr_err(DRIVER_NAME ": Failed to get child node");
			return err;
		}

		led->led_cdev.brightness_set = ll1202_brightness_set;
		led->led_cdev.brightness_get = ll1202_brightness_get;
		led->led_cdev.groups = ll1202_groups;
	}
	return 0;
}

static int ll1202_setup(struct ll1202_chip *chip)
{
	int ret;

	mutex_lock(&chip->lock);
	ret = ll1202_write_reg(chip, LL1202_DEV_ENABLE, 0x1);
	if (ret < 0) {
		dev_err(&chip->client->dev, "Failed writing to register [0x%x]\n",
			LL1202_DEV_ENABLE);
	}
	mutex_unlock(&chip->lock);
	usleep_range(6500, 10000);
	mutex_lock(&chip->lock);
	ret = ll1202_write_reg(chip, LL1202_DEV_ENABLE, 0x80);
	if (ret < 0) {
		dev_err(&chip->client->dev, "Failed writing to register [0x%x]\n",
			LL1202_DEV_ENABLE);
	}
	mutex_unlock(&chip->lock);
	usleep_range(6500, 10000);
	mutex_lock(&chip->lock);
	ret = ll1202_write_reg(chip, LL1202_PATTERN_REP, 0xFF);
	if (ret < 0) {
		dev_err(&chip->client->dev, "Failed writing to register [0x%x]\n",
			LL1202_PATTERN_REP);
		return ret;
	}
	mutex_unlock(&chip->lock);
	return ret;
}

static int ll1202_probe(struct i2c_client *client)
{
	struct ll1202_chip *chip;
	struct ll1202_led *led;
	int ret, err;
	int i;

	pr_info(DRIVER_NAME ": (I2C) " DRIVER_VERSION "\n");

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "SMBUS Byte Data not Supported\n");
		return -EIO;
	}

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	i2c_set_clientdata(client, chip);

	mutex_init(&chip->lock);
	chip->client = client;

	/* Device tree setup */
	ret = ll1202_dt_init(chip);
	if (ret < 0)
		goto exit;

	/* Configuration setup */
	ret = ll1202_setup(chip);
	if (ret < 0)
		goto exit;

	for (i = 0; i < LL1202_MAX_LEDS; i++) {
		led = &chip->leds[i];
		led->chip = chip;
		led->led_num = i;
		if (led->is_active) {
			err = led_classdev_register(&client->dev,
						    &led->led_cdev);
			if (err < 0) {
				pr_err(DRIVER_NAME
				       ": Failed to register LED class dev");
				goto exit;
			}
		}
	}

	ret = sysfs_create_group(&client->dev.kobj, &attr_group);
	if (ret) {
		dev_err(&client->dev,
			"Failed to create sysfs group for ll1202\n");
		goto err_setup;
	}

	return 0;

err_setup:
	for (i = 0; i < LL1202_MAX_LEDS; i++)
		led_classdev_unregister(&chip->leds[i].led_cdev);
exit:
	mutex_destroy(&chip->lock);
	devm_kfree(&client->dev, chip);
	return ret;
}

static void ll1202_remove(struct i2c_client *client)
{
	struct ll1202_chip *dev = i2c_get_clientdata(client);
	int i;

	for (i = 0; i < LL1202_MAX_LEDS; i++)
		led_classdev_unregister(&dev->leds[i].led_cdev);

	sysfs_remove_group(&client->dev.kobj, &attr_group);

	mutex_destroy(&dev->lock);
	devm_kfree(&client->dev, dev->leds);
	devm_kfree(&client->dev, dev);
}

static const struct i2c_device_id ll1202_id[] = {
	{ DRIVER_NAME "-i2c", 0 },
	{}
};

MODULE_DEVICE_TABLE(i2c, ll1202_id);

static const struct of_device_id ll1202_dt_ids[] = {
	{
		.compatible = "st,led1202",
	},
};

MODULE_DEVICE_TABLE(of, ll1202_dt_ids);

static struct i2c_driver ll1202_driver = {
	.driver = {
		.name = "ll1202",
		.of_match_table = of_match_ptr(ll1202_dt_ids),
	},
	.probe = ll1202_probe,
	.remove = ll1202_remove,
	.id_table = ll1202_id,
};

module_i2c_driver(ll1202_driver);

MODULE_AUTHOR("Remote Tech LTD");
MODULE_DESCRIPTION("LED1202 : 12-channel constant current LED driver");
MODULE_LICENSE("GPL");
