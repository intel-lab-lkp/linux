// SPDX-License-Identifier: GPL-2.0-only
/*
 * TCS3400 - AMS/TAOS color light sensor with RGBC and RGB-IR channels
 *
 * Copyright (c) 2025 Petr Hodina
 *
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>

#define TCS3400_DRV_NAME "tcs3400"
#define TCS3400_CMD_REG(reg) (0x80 | (reg))
#define TCS3400_CMD_SPECIAL 0xE0
#define TCS3400_CMD_ALS_INT_CLR 0xE6
#define TCS3400_CMD_ALL_INT_CLR 0xE7
#define TCS3400_ENABLE 0x00
#define TCS3400_ATIME 0x01
#define TCS3400_WTIME 0x03
#define TCS3400_PERSIST 0x0C
#define TCS3400_CONTROL 0x0F /* Gain */
#define TCS3400_STATUS 0x13
#define TCS3400_CDATAL 0x14 /* Clear low */
#define TCS3400_RDATAL 0x16
#define TCS3400_GDATAL 0x18
#define TCS3400_BDATAL 0x1A
#define TCS3400_ID 0x12
#define TCS3400_CHSEL 0xC0 /* Access IR channel: 0x00 RGBC, 0x80 RGB-IR */
#define TCS3400_EN_PON BIT(0)
#define TCS3400_EN_AEN BIT(1)
#define TCS3400_EN_AIEN BIT(4)
#define TCS3400_STATUS_AVALID BIT(0)
#define TCS3400_STATUS_AINT BIT(4)
#define TCS3400_GAIN_1X 0x00
#define TCS3400_GAIN_4X 0x01
#define TCS3400_GAIN_16X 0x02
#define TCS3400_GAIN_64X 0x03
#define TCS3400_MAX_ATIME 256

#define TCS3400_IIO_CHANNEL(_index, _mod) { \
	.type = IIO_INTENSITY, \
	.modified = 1, \
	.channel2 = IIO_MOD_##_mod, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			      BIT(IIO_CHAN_INFO_SCALE), \
			      BIT(IIO_CHAN_INFO_SCALE) | \
			      BIT(IIO_CHAN_INFO_INT_TIME), \
	.indexed = 1, \
	.channel = _index, \
	.scan_index = _index, \
	.scan_type = { \
		.sign = 'u', \
		.realbits = 16, \
		.storagebits = 16, \
		.endianness = IIO_LE, \
	}, \
}

struct tcs3400_data {
	struct i2c_client *client;
	struct mutex lock;
	struct regulator *vdd_supply;
	u8 atime;
	u8 gain;
	u8 channel_mode; /* 0x00 or 0x80 */
	u16 clear_ir; /* clear when mode=0x00, IR when mode=0x80 */
	u16 red;
	u16 green;
	u16 blue;
};

static const int tcs3400_gains[] = {1, 4, 16, 64};

static int tcs3400_power_on(struct tcs3400_data *data)
{
	int ret;

	ret = regulator_enable(data->vdd_supply);
	if (ret)
		return ret;

	msleep(20);

	return 0;
}

static void tcs3400_power_off(struct tcs3400_data *data)
{
	regulator_disable(data->vdd_supply);
}

static int tcs3400_write_reg(struct tcs3400_data *data, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(data->client, TCS3400_CMD_REG(reg), val);
}

static int tcs3400_read_reg(struct tcs3400_data *data, u8 reg, u8 *val)
{
	int ret = i2c_smbus_read_byte_data(data->client, TCS3400_CMD_REG(reg));

	if (ret < 0)
		return ret;
	*val = ret;

	return 0;
}

static int tcs3400_read_word(struct tcs3400_data *data, u8 reg, u16 *val)
{

	__le16 buf;
	int ret = i2c_smbus_read_i2c_block_data(data->client,
						TCS3400_CMD_REG(reg), 2, (u8 *)&buf);
	if (ret < 0)
		return ret;
	*val = le16_to_cpu(buf);
	return 0;
}
static int tcs3400_clear_interrupt(struct tcs3400_data *data)
{

	return i2c_smbus_write_byte(data->client, TCS3400_CMD_ALS_INT_CLR);
}

static int tcs3400_read_channels(struct tcs3400_data *data)
{

	int ret, retries = 20;
	u8 status;

	do {
		ret = tcs3400_read_reg(data, TCS3400_STATUS, &status);
		if (ret)
			return ret;
		if (status & TCS3400_STATUS_AVALID)
			break;
		usleep_range(5000, 6000);
	} while (--retries);
	if (!retries) {
		dev_warn(&data->client->dev, "Timeout waiting for valid data\n");
		return -ETIMEDOUT;
	}
	ret = tcs3400_read_word(data, TCS3400_CDATAL, &data->clear_ir);
	if (ret)
		return ret;

	ret = tcs3400_read_word(data, TCS3400_RDATAL, &data->red);
	if (ret)
		return ret;

	ret = tcs3400_read_word(data, TCS3400_GDATAL, &data->green);
	if (ret)
		return ret;

	ret = tcs3400_read_word(data, TCS3400_BDATAL, &data->blue);
	if (ret)
		return ret;
	return 0;
}

static irqreturn_t tcs3400_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct tcs3400_data *data = iio_priv(indio_dev);
	u16 buf[4];
	int ret;

	mutex_lock(&data->lock);
	ret = tcs3400_read_channels(data);
	if (!ret) {
		buf[0] = data->clear_ir;
		buf[1] = data->red;
		buf[2] = data->green;
		buf[3] = data->blue;
		iio_push_to_buffers_with_timestamp(indio_dev, buf,
						   iio_get_time_ns(indio_dev));
	}
	mutex_unlock(&data->lock);

	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static irqreturn_t tcs3400_irq_handler(int irq, void *priv)
{
	struct iio_dev *indio_dev = priv;
	struct tcs3400_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = tcs3400_read_channels(data);
	if (!ret)
		iio_trigger_poll_nested(indio_dev->trig);

	tcs3400_clear_interrupt(data);
	mutex_unlock(&data->lock);

	return IRQ_HANDLED;
}

static int tcs3400_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct tcs3400_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = tcs3400_read_channels(data);
	if (ret) {
		mutex_unlock(&data->lock);
		return ret;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->channel2) {
		case IIO_MOD_LIGHT_CLEAR:
			*val = data->clear_ir;
			break;
		case IIO_MOD_LIGHT_RED:
			*val = data->red;
			break;
		case IIO_MOD_LIGHT_GREEN:
			*val = data->green;
			break;
		case IIO_MOD_LIGHT_BLUE:
			*val = data->blue;
			break;
		default:
			ret = -EINVAL;
			break;
		}
		ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_INT_TIME:
		*val = 0;
		*val2 = (TCS3400_MAX_ATIME - data->atime) * 2780000; /* 2.78 ms per cycle */
		ret = IIO_VAL_INT_PLUS_MICRO;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&data->lock);

	return ret;
}

static int tcs3400_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct tcs3400_data *data = iio_priv(indio_dev);
	int i, ret = 0;

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		if (val != 0)
			return -EINVAL;
		i = TCS3400_MAX_ATIME - DIV_ROUND_CLOSEST(val2, 2780000);
		if (i < 1 || i >= TCS3400_MAX_ATIME)
			return -EINVAL;
		mutex_lock(&data->lock);
		data->atime = i;
		ret = tcs3400_write_reg(data, TCS3400_ATIME, data->atime);
		mutex_unlock(&data->lock);
		return ret;
	default:
		return -EINVAL;
	}
}

static const struct iio_chan_spec tcs3400_channels[] = {
	TCS3400_IIO_CHANNEL(0, LIGHT_CLEAR),
	TCS3400_IIO_CHANNEL(1, LIGHT_RED),
	TCS3400_IIO_CHANNEL(2, LIGHT_GREEN),
	TCS3400_IIO_CHANNEL(3, LIGHT_BLUE),
	IIO_CHAN_SOFT_TIMESTAMP(4),
};

static ssize_t tcs3400_enable_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct tcs3400_data *data = iio_priv(indio_dev);
	u8 enable;
	int ret;

	ret = tcs3400_read_reg(data, TCS3400_ENABLE, &enable);
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", !!(enable & (TCS3400_EN_PON | TCS3400_EN_AEN)));
}

static ssize_t tcs3400_enable_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct tcs3400_data *data = iio_priv(indio_dev);
	bool enable;
	int ret;
	u8 val;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	mutex_lock(&data->lock);
	if (enable)
		val = TCS3400_EN_PON | TCS3400_EN_AEN;
	else
		val = 0;
	ret = tcs3400_write_reg(data, TCS3400_ENABLE, val);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	if (enable)
		msleep(20);
	return len;
}

static ssize_t tcs3400_ir_mode_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct tcs3400_data *data = iio_priv(indio_dev);

	return sprintf(buf, "%d\n", !!data->channel_mode);
}

static ssize_t tcs3400_ir_mode_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct tcs3400_data *data = iio_priv(indio_dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	mutex_lock(&data->lock);
	data->channel_mode = enable ? 0x80 : 0x00;
	ret = tcs3400_write_reg(data, TCS3400_CHSEL, data->channel_mode);
	mutex_unlock(&data->lock);
	return ret ? ret : len;
}

static IIO_DEVICE_ATTR(enable, 0644, tcs3400_enable_show, tcs3400_enable_store, 0);
static IIO_DEVICE_ATTR(ir_mode, 0644, tcs3400_ir_mode_show, tcs3400_ir_mode_store, 0);

static struct attribute *tcs3400_attributes[] = {
	&iio_dev_attr_enable.dev_attr.attr,
	&iio_dev_attr_ir_mode.dev_attr.attr,
	NULL
};

static const struct attribute_group tcs3400_attribute_group = {
	.attrs = tcs3400_attributes,
};

static const struct iio_info tcs3400_info = {
	.read_raw = tcs3400_read_raw,
	.write_raw = tcs3400_write_raw,
	.attrs = &tcs3400_attribute_group,
};

static int tcs3400_probe(struct i2c_client *client)
{
	struct tcs3400_data *data;
	struct iio_dev *indio_dev;
	int ret;
	u8 id;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	mutex_init(&data->lock);

	i2c_set_clientdata(client, indio_dev);

	data->vdd_supply = devm_regulator_get(&client->dev, "vdd");
	if (IS_ERR(data->vdd_supply))
		return dev_err_probe(&client->dev, PTR_ERR(data->vdd_supply),
				     "Unable to get VDD regulator\n");

	ret = tcs3400_power_on(data);
	if (ret)
		goto err_power_off;

	ret = i2c_smbus_read_byte_data(client, TCS3400_CMD_REG(TCS3400_ID));
	if (ret < 0) {
		ret = -ENODEV;
		goto err_power_off;
		return ret;
	}

	id = ret;
	if (id == 0x90)
		dev_info(&client->dev, "TCS3401/5 Chip ID: 0x%02x\n", id);
	if (id == 0x93)
		dev_info(&client->dev, "TCS3403/7 Chip ID: 0x%02x\n", id);
	else {
		dev_err(&client->dev, "Unknown chip ID: 0x%02x\n", id);
		ret = -ENODEV;
	}

	data->atime = 0xF6; /* ~27.8 ms integration */
	data->gain = TCS3400_GAIN_1X;
	data->channel_mode = 0x00;

	tcs3400_write_reg(data, TCS3400_ATIME, data->atime);
	tcs3400_write_reg(data, TCS3400_CONTROL, data->gain);
	tcs3400_write_reg(data, TCS3400_PERSIST, 0x00); /* interrupt every cycle */
	tcs3400_write_reg(data, TCS3400_CHSEL, data->channel_mode); /* RGBC mode */

	tcs3400_write_reg(data, TCS3400_ENABLE,
			  TCS3400_EN_PON | TCS3400_EN_AEN | TCS3400_EN_AIEN);

	indio_dev->name = TCS3400_DRV_NAME;
	indio_dev->channels = tcs3400_channels;
	indio_dev->num_channels = ARRAY_SIZE(tcs3400_channels);
	indio_dev->info = &tcs3400_info;
	indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_TRIGGERED;

	ret = devm_iio_triggered_buffer_setup(&client->dev, indio_dev,
					      NULL,
					      tcs3400_trigger_handler,
					      NULL);
	if (ret)
		goto err_power_off;
	if (client->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, tcs3400_irq_handler,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						"tcs3400_irq", indio_dev);
		if (ret)
			goto err_power_off;
	}

	ret = devm_iio_device_register(&client->dev, indio_dev);
	if (ret)
		goto err_power_off;
	return 0;
err_power_off:
	tcs3400_write_reg(data, TCS3400_ENABLE, 0);
	tcs3400_power_off(data);
	return ret;
}

static void tcs3400_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct tcs3400_data *data = iio_priv(indio_dev);

	tcs3400_write_reg(data, TCS3400_ENABLE, 0);
	tcs3400_power_off(data);
}

static const struct of_device_id tcs3400_of_match[] = {
	{ .compatible = "ams,tcs3400" },
	{ }
};

MODULE_DEVICE_TABLE(of, tcs3400_of_match);

static const struct i2c_device_id tcs3400_id[] = {
	{ "tcs3400", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, tcs3400_id);

static struct i2c_driver tcs3400_driver = {
	.driver = {
		.name = TCS3400_DRV_NAME,
		.of_match_table = tcs3400_of_match,
	},
	.probe = tcs3400_probe,
	.remove = tcs3400_remove,
	.id_table = tcs3400_id,
};

module_i2c_driver(tcs3400_driver);
MODULE_AUTHOR("Petr Hodina <petr.hodina@protonmail.com>");
MODULE_DESCRIPTION("AMS TCS3400 RGB/IR color sensor IIO driver");
MODULE_LICENSE("GPL");
