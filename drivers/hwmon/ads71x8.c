// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ads71x8.c - driver for TI ADS71x8 8-channel A/D converter and compatibles
 *
 * For further information, see the Documentation/hwmon/ads71x8.rst file.
 */

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#define MODULE_NAME "ads71x8"

/* AVDD (VREF) operating range */
#define ADS71x8_AVDD_MV_MIN		2350
#define ADS71x8_AVDD_MV_MAX		5500

/* ADS71x8 operation codes */
#define ADS71x8_OPCODE_WRITE		0x08
#define ADS71x8_OPCODE_SET_BIT		0x18
#define ADS71x8_OPCODE_BLOCK_WRITE	0x28
#define ADS71x8_OPCODE_BLOCK_READ	0x30

/* ADS71x8 registers */
#define ADS71x8_REG_GENERAL_CFG		0x01
#define ADS71x8_REG_OSR_CFG		0x03
#define ADS71x8_REG_OPMODE_CFG		0x04
#define ADS71x8_REG_SEQUENCE_CFG	0x10
#define ADS71x8_REG_CHANNEL_SEL		0x11
#define ADS71x8_REG_AUTO_SEQ_CH_SEL	0x12
#define ADS71x8_REG_ALERT_CH_SEL	0x14
#define ADS71x8_REG_EVENT_FLAG		0x18
#define ADS71x8_REG_EVENT_HIGH_FLAG	0x1A
#define ADS71x8_REG_EVENT_LOW_FLAG	0x1C
#define ADS71x8_REG_HIGH_TH_CH0		0x21
#define ADS71x8_REG_LOW_TH_CH0		0x23
#define ADS71x8_REG_MAX_CH0_LSB		0x60
#define ADS71x8_REG_MIN_CH0_LSB		0x80
#define ADS71x8_REG_RECENT_CH0_LSB	0xA0

/*
 * Modes after ADS71x8_MODE_MAX can't be selected by configuration
 * and are only intended for internal use of the driver.
 */
enum ads71x8_modes { ADS71x8_MODE_MANUAL, ADS71x8_MODE_AUTO,
	ADS71x8_MODE_MAX, ADS71x8_MODE_AUTO_IRQ };

/* Client specific data */
struct ads71x8_data {
	const struct i2c_device_id *id;
	struct i2c_client *client;
	struct device *hwmon_dev;
	int vref; /* Reference voltage in mV */
	struct mutex lock;
	u8 mode;
	u16 interval_us; /* Interval in us a new conversion is triggered */
	long alarms; /* State of window comparator events */
};

struct ads71x8_val_map {
	u16 val;
	u8 bits;
};

static const struct ads71x8_val_map ads71x8_intervals_us[] = {
	{ 1, 0x0 }, { 2, 0x02 }, { 3, 0x03 }, { 4, 0x04 }, { 6, 0x05 },
	{ 8, 0x06 }, { 12, 0x07 }, { 16, 0x08 }, { 24, 0x09 }, { 32, 0x10 },
	{ 48, 0x11 }, { 64, 0x12 }, { 96, 0x13 }, { 128, 0x14 },
	{ 192, 0x15 }, { 256, 0x16 }, { 384, 0x17 }, { 512, 0x18 },
	{ 768, 0x19 }, { 1024, 0x1A }, { 1536, 0x1B }, { 2048, 0x1C },
	{ 3072, 0x1D }, { 4096, 0x1E }, { 6144, 0x1F }
};

/* List of supported devices */
enum ads71x8_chips { ads7128, ads7138 };

static const struct i2c_device_id ads71x8_device_ids[] = {
	{ "ads7128", ads7128 },
	{ "ads7138", ads7138 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ads71x8_device_ids);

static const struct of_device_id __maybe_unused ads71x8_of_match[] = {
	{
		.compatible = "ti,ads7128",
		.data = (void *)ads7128
	},
	{
		.compatible = "ti,ads7138",
		.data = (void *)ads7138
	},
	{ },
};
MODULE_DEVICE_TABLE(of, ads71x8_of_match);

static int ads71x8_i2c_write_block(const struct i2c_client *client, u8 reg,
	u8 *values, u8 length)
{
	struct ads71x8_data *data = i2c_get_clientdata(client);
	int ret;
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = length + 2, /* "+ 2" for OPCODE and reg */
		},
	};

	msgs[0].buf = kmalloc(msgs[0].len, GFP_KERNEL);
	if (!msgs[0].buf)
		return -ENOMEM;

	msgs[0].buf[0] = ADS71x8_OPCODE_BLOCK_WRITE;
	msgs[0].buf[1] = reg;
	memcpy(&msgs[0].buf[2], values, length);

	mutex_lock(&data->lock);
	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	mutex_unlock(&data->lock);
	kfree(msgs[0].buf);

	return ret;
}

static int ads71x8_i2c_write(const struct i2c_client *client, u8 reg, u8 value)
{
	return ads71x8_i2c_write_block(client, reg, &value, sizeof(value));
}

static int ads71x8_i2c_set_bit(const struct i2c_client *client, u8 reg, u8 bits)
{
	struct ads71x8_data *data = i2c_get_clientdata(client);
	int ret;
	u8 buf[3] = {ADS71x8_OPCODE_SET_BIT, reg, bits};
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = ARRAY_SIZE(buf),
			.buf = buf,
		},
	};

	mutex_lock(&data->lock);
	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	mutex_unlock(&data->lock);

	return ret;
}

static int ads71x8_i2c_read_block(const struct i2c_client *client, u8 reg,
	u8 *out_values, u8 length)
{
	struct ads71x8_data *data = i2c_get_clientdata(client);
	int ret;
	u8 buf[2] = {ADS71x8_OPCODE_BLOCK_READ, reg};
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = ARRAY_SIZE(buf),
			.buf = buf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = out_values,
		},
	};

	mutex_lock(&data->lock);
	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	mutex_unlock(&data->lock);

	return ret;
}

static int ads71x8_i2c_read(const struct i2c_client *client, u8 reg)
{
	u8 value;
	int ret = ads71x8_i2c_read_block(client, reg, &value, sizeof(value));

	if (ret < 0)
		return ret;
	return value;
}

static int ads71x8_i2c_read_manual(const struct i2c_client *client, u8 channel,
	u16 *out_value)
{
	struct ads71x8_data *data = i2c_get_clientdata(client);
	int ret;
	u8 buf[3] = {ADS71x8_OPCODE_WRITE, ADS71x8_REG_CHANNEL_SEL, channel};
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = ARRAY_SIZE(buf),
			.buf = buf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 2,
			.buf = buf,
		},
	};

	mutex_lock(&data->lock);
	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	mutex_unlock(&data->lock);

	/*
	 * For manual reading the order of LSB and MSB is swapped in comparison
	 * to the other registers.
	 */
	*out_value = ((buf[0] << 8) | buf[1]);

	return ret;
}

static int ads71x8_read_input_mv(struct ads71x8_data *data, u8 reg, long *val)
{
	u8 values[2];
	int ret;

	ret = ads71x8_i2c_read_block(data->client, reg, values,
		ARRAY_SIZE(values));
	if (ret < 0)
		return ret;

	/*
	 * Mask the lowest 4 bits, because it has to be masked for some
	 * registers and doesn't change anything in the result, anyway.
	 */
	*val = ((values[1] << 8) | (values[0] & 0xF0));
	/*
	 * Standard resolution is 12 bit, but can get 16 bit if oversampling is
	 * enabled. Therefore, use 16 bit all the time, because the registers
	 * are aligned like that anyway.
	 */
	*val = DIV_ROUND_CLOSEST(*val * data->vref, (1 << 16));
	return 0;
}

static int ads71x8_read(struct device *dev, enum hwmon_sensor_types type,
	u32 attr, int channel, long *val)
{
	struct ads71x8_data *data = dev_get_drvdata(dev);
	u8 reg, values[2];
	u16 tmp_val;
	int ret;

	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_samples:
			ret = ads71x8_i2c_read(data->client, ADS71x8_REG_OSR_CFG);
			if (ret < 0)
				return ret;
			*val = (1 << (ret & 0x07));
			return 0;
		case hwmon_chip_update_interval:
			*val = data->interval_us;
			return 0;
		case hwmon_chip_alarms:
			*val = data->alarms;
			/* Reset alarms after reading */
			data->alarms = 0;
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			if (data->mode == ADS71x8_MODE_MANUAL) {
				ret = ads71x8_i2c_read_manual(data->client,
					channel, &tmp_val);
				*val = tmp_val;
			} else {
				reg = ADS71x8_REG_RECENT_CH0_LSB + (2 * channel);
				ret = ads71x8_i2c_read_block(data->client, reg,
					values, ARRAY_SIZE(values));
				*val = ((values[1] << 8) | values[0]);
			}
			if (ret < 0)
				return ret;
			*val = DIV_ROUND_CLOSEST(*val * data->vref, (1 << 16));
			return 0;
		case hwmon_in_min:
			reg = ADS71x8_REG_MIN_CH0_LSB + (2 * channel);
			return ads71x8_read_input_mv(data, reg, val);
		case hwmon_in_max:
			reg = ADS71x8_REG_MAX_CH0_LSB + (2 * channel);
			return ads71x8_read_input_mv(data, reg, val);
		case hwmon_in_min_alarm:
			reg = ADS71x8_REG_LOW_TH_CH0 - 1 + (4 * channel);
			return ads71x8_read_input_mv(data, reg, val);
		case hwmon_in_max_alarm:
			reg = ADS71x8_REG_HIGH_TH_CH0 - 1 + (4 * channel);
			return ads71x8_read_input_mv(data, reg, val);
		default:
			return -EOPNOTSUPP;
		}

	default:
		return -EOPNOTSUPP;
	}
}

static u32 get_closest_log2(u32 val)
{
	u32 down = ilog2(val);
	u32 up = ilog2(roundup_pow_of_two(val));

	return (val - (1 << down) < (1 << up) - val) ? down : up;
}

static int ads71x8_write(struct device *dev, enum hwmon_sensor_types type,
	u32 attr, int channel, long val)
{
	struct ads71x8_data *data = dev_get_drvdata(dev);
	u8 reg, values[2];
	int ret;

	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_samples:
			/* Number of samples can only be a power of 2 */
			values[0] = get_closest_log2(clamp_val(val, 1, 128));
			ret = ads71x8_i2c_write(data->client,
				ADS71x8_REG_OSR_CFG, values[0]);
			return ret < 0 ? ret : 0;
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_in:
		switch (attr) {
		case hwmon_in_min_alarm:
			reg = ADS71x8_REG_LOW_TH_CH0 - 1 + (4 * channel);
			val = DIV_ROUND_CLOSEST(val * (1 << 16), data->vref);
			val = clamp_val(val, 0, 65535);
			values[0] = (val & 0xF0);
			values[1] = (val >> 8) & 0xFF;
			ret = ads71x8_i2c_write_block(data->client, reg,
				values, ARRAY_SIZE(values));
			return ret < 0 ? ret : 0;
		case hwmon_in_max_alarm:
			reg = ADS71x8_REG_HIGH_TH_CH0 - 1 + (4 * channel);
			val = DIV_ROUND_CLOSEST(val * (1 << 16), data->vref);
			val = clamp_val(val, 0, 65535);
			values[0] = (val & 0xF0);
			values[1] = (val >> 8) & 0xFF;
			ret = ads71x8_i2c_write_block(data->client, reg,
				values, ARRAY_SIZE(values));
			return ret < 0 ? ret : 0;
		default:
			return -EOPNOTSUPP;
		}

	default:
		return -EOPNOTSUPP;
	}
}

static umode_t ads71x8_is_visible(const void *_data,
	enum hwmon_sensor_types type, u32 attr, int channel)
{
	u8 mode = ((struct ads71x8_data *)_data)->mode;

	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_samples:
			return 0644;
		case hwmon_chip_update_interval:
			return mode >= ADS71x8_MODE_AUTO ? 0444 : 0;
		case hwmon_chip_alarms:
			return mode >= ADS71x8_MODE_AUTO_IRQ ? 0444 : 0;
		default:
			return 0;
		}

	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			return 0444;
		case hwmon_in_min:
		case hwmon_in_max:
			return mode >= ADS71x8_MODE_AUTO ? 0444 : 0;
		case hwmon_in_min_alarm:
		case hwmon_in_max_alarm:
			return mode >= ADS71x8_MODE_AUTO_IRQ ? 0644 : 0;
		default:
			return 0;
		}

	default:
		return 0;
	}
}

static const struct hwmon_channel_info *ads71x8_info[] = {
	HWMON_CHANNEL_INFO(chip,
		HWMON_C_SAMPLES | HWMON_C_ALARMS | HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(in,
		HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM,
		HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM,
		HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM,
		HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM,
		HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM,
		HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM,
		HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM,
		HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM),
	NULL
};

static const struct hwmon_ops ads71x8_hwmon_ops = {
	.is_visible = ads71x8_is_visible,
	.read = ads71x8_read,
	.write = ads71x8_write,
};

static const struct hwmon_chip_info ads71x8_chip_info = {
	.ops = &ads71x8_hwmon_ops,
	.info = ads71x8_info,
};

static ssize_t ads71x8_cal_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	struct ads71x8_data *data = dev_get_drvdata(dev);
	int ret;

	ret = ads71x8_i2c_read(data->client, ADS71x8_REG_GENERAL_CFG);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%d\n", (ret & 0x02));
}

static ssize_t ads71x8_cal_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct ads71x8_data *data = dev_get_drvdata(dev);
	int ret;
	long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val == 0)
		return count;

	ret = ads71x8_i2c_set_bit(data->client, ADS71x8_REG_GENERAL_CFG, 0x02);
	if (ret < 0)
		return ret;

	return count;
}

static SENSOR_DEVICE_ATTR_RW(calibrate, ads71x8_cal, 0);

static struct attribute *ads71x8_attrs[] = {
	&sensor_dev_attr_calibrate.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(ads71x8);

static const struct ads71x8_val_map *get_closest_interval(u16 freq)
{
	const int idx_max = ARRAY_SIZE(ads71x8_intervals_us) - 1;
	u16 cur, best = ads71x8_intervals_us[idx_max].val;
	int i;

	freq = clamp_val(freq, ads71x8_intervals_us[0].val,
		ads71x8_intervals_us[idx_max].val);

	for (i = 0; i <= idx_max; i++) {
		cur = abs(ads71x8_intervals_us[i].val - freq);
		if (cur > best)
			return &ads71x8_intervals_us[i-1];
		best = cur;
	}
	return &ads71x8_intervals_us[0];
}

static irqreturn_t ads71x8_irq_handler(int irq, void *_data)
{
	struct ads71x8_data *data = _data;
	struct device *dev = &data->client->dev;
	int ret;

	ret = ads71x8_i2c_read(data->client, ADS71x8_REG_EVENT_FLAG);
	if (ret <= 0)
		return IRQ_NONE;

	ret = ads71x8_i2c_read(data->client, ADS71x8_REG_EVENT_HIGH_FLAG);
	if (ret < 0)
		goto out;
	data->alarms |= (ret << 8);

	ret = ads71x8_i2c_read(data->client, ADS71x8_REG_EVENT_LOW_FLAG);
	if (ret < 0)
		goto out;
	data->alarms |= (ret);

	/* Clear all interrupt flags, so next interrupt can be captured */
	ret = ads71x8_i2c_write(data->client, ADS71x8_REG_EVENT_HIGH_FLAG, 0xFF);
	if (ret < 0)
		goto out;
	ret = ads71x8_i2c_write(data->client, ADS71x8_REG_EVENT_LOW_FLAG, 0xFF);
	if (ret < 0)
		goto out;

	/* Notify poll/select in userspace. CONFIG_SYSFS must be set! */
	sysfs_notify(&data->hwmon_dev->kobj, NULL, "alarms");
	kobject_uevent(&data->hwmon_dev->kobj, KOBJ_CHANGE);

out:
	if (ret < 0)
		dev_warn(dev, "couldn't handle interrupt correctly: %d\n", ret);
	return IRQ_HANDLED;
}

static int ads71x8_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ads71x8_data *data;
	struct device *hwmon_dev;
	struct regulator *regulator;
	const struct ads71x8_val_map *interval = ads71x8_intervals_us;
	int vref, ret, err = 0;

	data = devm_kzalloc(dev, sizeof(struct ads71x8_data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		goto out;
	}

	data->client = client;
	data->id = i2c_match_id(ads71x8_device_ids, client);
	i2c_set_clientdata(client, data);
	mutex_init(&data->lock);

	/* Reset the chip to get a defined starting configuration */
	ret = ads71x8_i2c_write(data->client, ADS71x8_REG_GENERAL_CFG, 0x01);
	if (ret < 0) {
		dev_err(dev, "failed to reset\n");
		err = ret;
		goto cleanup_mutex;
	}

	/* Get AVDD (in mv) which is the analog supply and reference voltage */
	regulator = devm_regulator_get(dev, "avdd");
	if (IS_ERR(regulator)) {
		err = PTR_ERR(regulator);
		goto cleanup_mutex;
	}

	vref = regulator_get_voltage(regulator);
	data->vref = DIV_ROUND_CLOSEST(vref, 1000);
	if (data->vref < ADS71x8_AVDD_MV_MIN || data->vref > ADS71x8_AVDD_MV_MAX) {
		dev_err(dev, "invalid value for AVDD %d\n", data->vref);
		err = -EINVAL;
		goto cleanup_mutex;
	}

	/*
	 * Try reading optional parameter 'ti,mode', otherwise keep current
	 * mode, which is manual mode.
	 */
	if (of_property_read_u8(dev->of_node, "ti,mode", &data->mode) == 0) {
		if (data->mode >= ADS71x8_MODE_MAX) {
			dev_err(dev, "invalid operation mode %d\n", data->mode);
			err = -EINVAL;
			goto cleanup_mutex;
		}
	}

	if (data->mode <= ADS71x8_MODE_MANUAL)
		goto conf_manual;

	/* Try reading optional parameter 'ti,interval' */
	if (of_property_read_u16(dev->of_node, "ti,interval", &data->interval_us) == 0)
		interval = get_closest_interval(data->interval_us);
	data->interval_us = interval->val;

	/* Check if interrupt is also configured */
	if (!client->irq) {
		dev_warn(dev, "interrupt not available, intended?\n");
		goto conf_auto;
	}

	data->mode = ADS71x8_MODE_AUTO_IRQ;
	ret = devm_request_threaded_irq(&client->dev, client->irq,
		NULL, ads71x8_irq_handler,
		IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_SHARED,
		NULL, data);
	if (ret) {
		dev_err(dev, "unable to request IRQ %d\n", client->irq);
		err = ret;
		goto cleanup_mutex;
	}

	/* Enable possibility to trigger an alert/interrupt for all channels */
	ret = ads71x8_i2c_write(data->client, ADS71x8_REG_ALERT_CH_SEL, 0xFF);
	if (ret < 0) {
		err = ret;
		goto cleanup_config;
	}

conf_auto:
	/* Set to autonomous conversion and update interval */
	ret = ads71x8_i2c_write(data->client, ADS71x8_REG_OPMODE_CFG,
		0b00100000 | (interval->bits & 0x1F));
	if (ret < 0) {
		err = ret;
		goto cleanup_config;
	}

	/* Enable statistics and digital window comparator */
	ret = ads71x8_i2c_write(data->client, ADS71x8_REG_GENERAL_CFG,
		0b00110000);
	if (ret < 0) {
		err = ret;
		goto cleanup_config;
	}

	/* Enable all channels for auto sequencing */
	ret = ads71x8_i2c_write(data->client, ADS71x8_REG_AUTO_SEQ_CH_SEL,
		0xFF);
	if (ret < 0) {
		err = ret;
		goto cleanup_config;
	}

	/* Set auto sequence mode and start sequencing */
	ret = ads71x8_i2c_write(data->client, ADS71x8_REG_SEQUENCE_CFG,
		0b00010001);
	if (ret < 0) {
		err = ret;
		goto cleanup_config;
	}

conf_manual:
	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
		data, &ads71x8_chip_info, ads71x8_groups);
	if (IS_ERR_OR_NULL(hwmon_dev)) {
		err = PTR_ERR_OR_ZERO(hwmon_dev);
		goto cleanup_mutex;
	}
	data->hwmon_dev = hwmon_dev;

	goto out;

cleanup_config:
	dev_err(dev, "failed to configure IC: %d\n", err);
cleanup_mutex:
	mutex_destroy(&data->lock);
out:
	return err;
}

static void ads71x8_remove(struct i2c_client *client)
{
	struct ads71x8_data *data = i2c_get_clientdata(client);

	/* Reset the chip */
	if (ads71x8_i2c_write(data->client, ADS71x8_REG_GENERAL_CFG, 0x01) < 0)
		dev_err(&client->dev, "failed to reset\n");

	mutex_destroy(&data->lock);
}

static struct i2c_driver ads71x8_driver = {
	.driver = {
		.name = MODULE_NAME,
		.of_match_table = of_match_ptr(ads71x8_of_match),
	},
	.id_table = ads71x8_device_ids,
	.probe = ads71x8_probe,
	.remove = ads71x8_remove,
};
/* Cares about module_init and _exit */
module_i2c_driver(ads71x8_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobias Sperling <tobias.sperling@softing.com>");
MODULE_DESCRIPTION("Driver for TI ADS71x8 ADCs");
