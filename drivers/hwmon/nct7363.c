// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023 Nuvoton Technology corporation.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define NCT7363_REG_GPIO_0_3		0x20
#define NCT7363_REG_GPIO_4_7		0x21
#define NCT7363_REG_GPIO_10_13		0x22
#define NCT7363_REG_GPIO_14_17		0x23
#define NCT7363_REG_PWMEN_0_7		0x38
#define NCT7363_REG_PWMEN_8_15		0x39
#define NCT7363_REG_FANINEN_0_7		0x41
#define NCT7363_REG_FANINEN_8_15	0x42
#define NCT7363_REG_FANINx_HVAL(x)	(0x48 + ((x) * 2))
#define NCT7363_REG_FANINx_LVAL(x)	(0x49 + ((x) * 2))
#define NCT7363_REG_FSCPxDUTY(x)	(0x90 + ((x) * 2))
#define NCT7363_REG_VENDOR_ID		0xFD
#define NCT7363_REG_CHIP_ID		0xFE
#define NCT7363_REG_DEVICE_ID		0xFF

#define NUVOTON_ID			0x49
#define CHIP_ID				0x19
#define DEVICE_ID			0x88

#define PWM_SEL(x)			(BIT(0) << ((x % 4) * 2))
#define FANIN_SEL(x)			(BIT(1) << ((x % 4) * 2))
#define BIT_CHECK(x)			(BIT(0) << x)

#define NCT7363_FANINx_LVAL_MASK	GENMASK(4, 0)
#define NCT7363_FANIN_MASK		GENMASK(12, 0)

#define NCT7363_PWM_COUNT		16
#define NCT7363_FANIN_COUNT		16

#define REFRESH_INTERVAL		(2 * HZ)

static inline unsigned long FAN_FROM_REG(u16 val)
{
	if ((val >= NCT7363_FANIN_MASK) || (val == 0))
		return	0;

	return (1350000UL / val);
}

static const unsigned short normal_i2c[] = {
	0x20, 0x21, 0x22, 0x23, I2C_CLIENT_END
};

enum chips { nct7363 };

static const struct i2c_device_id nct7363_id[] = {
	{ "nct7363", nct7363 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nct7363_id);

static const struct of_device_id nct7363_of_match[] = {
	{ .compatible = "nuvoton,nct7363", .data = (void *)nct7363 },
	{ },
};
MODULE_DEVICE_TABLE(of, nct7363_of_match);

struct nct7363_data {
	struct regmap		*regmap;
	struct mutex		update_lock;
	bool			valid;
	unsigned long		last_updated; /* In jiffies */

	u16			fanin_mask;
	u16			fan[NCT7363_FANIN_COUNT];
	u16			pwm_mask;
	u8			pwm[NCT7363_PWM_COUNT];
};

static struct nct7363_data *nct7363_update_device(struct device *dev)
{
	struct nct7363_data *data = dev_get_drvdata(dev);
	unsigned int hi, lo, regval;
	int i, ret = 0;

	mutex_lock(&data->update_lock);

	if (!(time_after(jiffies, data->last_updated + REFRESH_INTERVAL)
	      || !data->valid))
		goto no_sensor_update;

	for (i = 0; i < ARRAY_SIZE(data->fan); i++) {
		if (!(data->fanin_mask & BIT_CHECK(i)))
			continue;

		/*
		 * High-byte register should be read first to latch
		 * synchronous low-byte value
		 */
		ret = regmap_read(data->regmap,
				  NCT7363_REG_FANINx_HVAL(i), &hi);
		if (ret)
			goto error;

		ret = regmap_read(data->regmap,
				  NCT7363_REG_FANINx_LVAL(i), &lo);
		if (ret)
			goto error;

		data->fan[i] = (hi << 5) | (lo & NCT7363_FANINx_LVAL_MASK);
	}

	for (i = 0; i < ARRAY_SIZE(data->pwm); i++) {
		if (!(data->pwm_mask & BIT_CHECK(i)))
			continue;

		ret = regmap_read(data->regmap,
				  NCT7363_REG_FSCPxDUTY(i), &regval);
		if (ret)
			goto error;

		data->pwm[i] = regval;
	}

	data->last_updated = jiffies;
	data->valid = true;

error:
	if (ret)
		data = ERR_PTR(ret);

no_sensor_update:
	mutex_unlock(&data->update_lock);

	return data;
}

static int nct7363_read_fan(struct device *dev, u32 attr, int channel,
			    long *val)
{
	struct nct7363_data *data = nct7363_update_device(dev);
	u16 cnt, rpm;

	if (IS_ERR(data))
		return PTR_ERR(data);

	switch (attr) {
	case hwmon_fan_input:
		cnt = data->fan[channel] & NCT7363_FANIN_MASK;
		rpm = FAN_FROM_REG(cnt);
		*val = (long)rpm;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t nct7363_fan_is_visible(const void *_data, u32 attr, int channel)
{
	const struct nct7363_data *data = _data;

	switch (attr) {
	case hwmon_fan_input:
		if (data->fanin_mask & BIT_CHECK(channel))
			return 0444;
		break;
	default:
		break;
	}

	return 0;
}

static int nct7363_read_pwm(struct device *dev, u32 attr, int channel,
			    long *val)
{
	struct nct7363_data *data = nct7363_update_device(dev);
	u16 ret;

	if (IS_ERR(data))
		return PTR_ERR(data);

	switch (attr) {
	case hwmon_pwm_input:
		ret = data->pwm[channel];
		*val = (long)ret;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int nct7363_write_pwm(struct device *dev, u32 attr, int channel,
			     long val)
{
	struct nct7363_data *data = nct7363_update_device(dev);
	int ret;

	if (IS_ERR(data))
		return PTR_ERR(data);

	switch (attr) {
	case hwmon_pwm_input:
		if (val < 0 || val > 255)
			return -EINVAL;
		mutex_lock(&data->update_lock);
		ret = regmap_write(data->regmap,
				   NCT7363_REG_FSCPxDUTY(channel), val);
		if (ret == 0)
			data->pwm[channel] = val;
		mutex_unlock(&data->update_lock);
		return ret;

	default:
		return -EOPNOTSUPP;
	}
}

static umode_t nct7363_pwm_is_visible(const void *_data, u32 attr, int channel)
{
	const struct nct7363_data *data = _data;

	switch (attr) {
	case hwmon_pwm_input:
		if (data->pwm_mask & BIT_CHECK(channel))
			return 0644;
		break;
	default:
		break;
	}

	return 0;
}

static int nct7363_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_fan:
		return nct7363_read_fan(dev, attr, channel, val);
	case hwmon_pwm:
		return nct7363_read_pwm(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int nct7363_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_pwm:
		return nct7363_write_pwm(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t nct7363_is_visible(const void *data,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		return nct7363_fan_is_visible(data, attr, channel);
	case hwmon_pwm:
		return nct7363_pwm_is_visible(data, attr, channel);
	default:
		return 0;
	}
}

static const struct hwmon_channel_info *nct7363_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_ops nct7363_hwmon_ops = {
	.is_visible = nct7363_is_visible,
	.read = nct7363_read,
	.write = nct7363_write,
};

static const struct hwmon_chip_info nct7363_chip_info = {
	.ops = &nct7363_hwmon_ops,
	.info = nct7363_info,
};

/* Return 0 if detection is successful, -ENODEV otherwise */
static int nct7363_detect(struct i2c_client *client,
			  struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
	int vendor, chip, device;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	vendor = i2c_smbus_read_byte_data(client, NCT7363_REG_VENDOR_ID);
	if (vendor != NUVOTON_ID)
		return -ENODEV;

	chip = i2c_smbus_read_byte_data(client, NCT7363_REG_CHIP_ID);
	if (chip != CHIP_ID)
		return -ENODEV;

	device = i2c_smbus_read_byte_data(client, NCT7363_REG_DEVICE_ID);
	if (device != DEVICE_ID)
		return -ENODEV;

	strscpy(info->type, "nct7363", I2C_NAME_SIZE);

	return 0;
}

static int nct7363_init_chip(struct nct7363_data *data)
{
	u8 i, gpio0_3 = 0, gpio4_7 = 0, gpio10_13 = 0, gpio14_17 = 0;
	int ret;

	for (i = 0; i < NCT7363_PWM_COUNT; i++) {
		if (i < 4) {
			if (data->pwm_mask & BIT_CHECK(i))
				gpio0_3 |= PWM_SEL(i);
			if (data->fanin_mask & BIT_CHECK(i))
				gpio10_13 |= FANIN_SEL(i);
		} else if (i < 8) {
			if (data->pwm_mask & BIT_CHECK(i))
				gpio4_7 |= PWM_SEL(i);
			if (data->fanin_mask & BIT_CHECK(i))
				gpio14_17 |= FANIN_SEL(i);
		} else if (i < 12) {
			if (data->pwm_mask & BIT_CHECK(i))
				gpio10_13 |= PWM_SEL(i);
			if (data->fanin_mask & BIT_CHECK(i))
				gpio0_3 |= FANIN_SEL(i);
		} else {
			if (data->pwm_mask & BIT_CHECK(i))
				gpio14_17 |= PWM_SEL(i);
			if (data->fanin_mask & BIT_CHECK(i))
				gpio4_7 |= FANIN_SEL(i);
		}
	}

	/* Pin Function Configuration */
	ret = regmap_write(data->regmap, NCT7363_REG_GPIO_0_3, gpio0_3);
	if (ret < 0)
		return ret;
	ret = regmap_write(data->regmap, NCT7363_REG_GPIO_4_7, gpio4_7);
	if (ret < 0)
		return ret;
	ret = regmap_write(data->regmap, NCT7363_REG_GPIO_10_13, gpio10_13);
	if (ret < 0)
		return ret;
	ret = regmap_write(data->regmap, NCT7363_REG_GPIO_14_17, gpio14_17);
	if (ret < 0)
		return ret;

	/* PWM and FANIN Monitoring Enable */
	ret = regmap_write(data->regmap, NCT7363_REG_PWMEN_0_7,
			   data->pwm_mask & 0xff);
	if (ret < 0)
		return ret;
	ret = regmap_write(data->regmap, NCT7363_REG_PWMEN_8_15,
			   (data->pwm_mask >> 8) & 0xff);
	if (ret < 0)
		return ret;
	ret = regmap_write(data->regmap, NCT7363_REG_FANINEN_0_7,
			   data->fanin_mask & 0xff);
	if (ret < 0)
		return ret;
	ret = regmap_write(data->regmap, NCT7363_REG_FANINEN_8_15,
			   (data->fanin_mask >> 8) & 0xff);
	if (ret < 0)
		return ret;

	return 0;
}

static int nct7363_present_pwm_fanin(struct device *dev,
				     struct device_node *child,
				     struct nct7363_data *data)
{
	struct of_phandle_args args;
	int ret, fanin_cnt;
	u8 *fanin_ch;
	u8 ch, index;

	ret = of_parse_phandle_with_args(child, "pwms", "#pwm-cells",
					 0, &args);
	if (ret)
		return ret;

	data->pwm_mask |= BIT(args.args[0]);

	fanin_cnt = of_property_count_u8_elems(child, "tach-ch");
	if (fanin_cnt < 1)
		return -EINVAL;

	fanin_ch = devm_kcalloc(dev, fanin_cnt, sizeof(*fanin_ch), GFP_KERNEL);
	if (!fanin_ch)
		return -ENOMEM;

	ret = of_property_read_u8_array(child, "tach-ch", fanin_ch, fanin_cnt);
	if (ret)
		return ret;

	for (ch = 0; ch < fanin_cnt; ch++) {
		index = fanin_ch[ch];
		data->fanin_mask |= BIT(index);
	}

	return 0;
}

static const struct regmap_config nct7363_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int nct7363_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *child;
	struct nct7363_data *data;
	struct device *hwmon_dev;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regmap = devm_regmap_init_i2c(client, &nct7363_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	mutex_init(&data->update_lock);

	for_each_child_of_node(dev->of_node, child) {
		ret = nct7363_present_pwm_fanin(dev, child, data);
		if (ret) {
			of_node_put(child);
			return ret;
		}
	}

	/* Initialize the chip */
	ret = nct7363_init_chip(data);
	if (ret)
		return ret;

	hwmon_dev =
		devm_hwmon_device_register_with_info(dev, client->name, data,
						     &nct7363_chip_info, NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static struct i2c_driver nct7363_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name = "nct7363",
		.of_match_table = nct7363_of_match,
	},
	.probe = nct7363_probe,
	.id_table = nct7363_id,
	.detect = nct7363_detect,
	.address_list = normal_i2c,
};

module_i2c_driver(nct7363_driver);

MODULE_AUTHOR("CW Ho <cwho@nuvoton.com>");
MODULE_AUTHOR("Ban Feng <kcfeng0@nuvoton.com>");
MODULE_DESCRIPTION("NCT7363 Hardware Monitoring Driver");
MODULE_LICENSE("GPL");
