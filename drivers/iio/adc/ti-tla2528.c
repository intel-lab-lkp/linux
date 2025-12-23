// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Texas Instruments TLA2528 ADC
 *
 * Copyright (C) 2020-2021 Rodolfo Giometti <giometti@enneenne.com>
 * Copyright (C) 2025 Maxime Chevallier <maxime.chevallier@bootlin.com>
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>

#define TLA2528_OP_WRITE_REG		0x08

#define TLA2528_DATA_CFG_ADR		0x02

/* Datasheet says [5:4] sets the append status, but only bit 4 is used */
#define TLA2528_DATA_CFG_APPEND_STATUS	BIT(4)
#define TLA2528_PIN_CFG_ADR		0x05
#define TLA2528_SEQUENCE_CFG_ADR	0x10
#define TLA2528_CHANNEL_SEL_ADR		0x11

struct tla2528 {
	struct i2c_client *client;
	int vref_uv;

	/* Protects manual channel selection, i.e. last_read_channel */
	struct mutex lock;
	u8 last_read_channel;
};

static s32 tla2528_write_reg(const struct i2c_client *client, u8 reg, u8 val)
{
	u8 data[3] = {TLA2528_OP_WRITE_REG, reg, val};
	int ret;

	ret = i2c_master_send(client, data, 3);

	return ret < 0 ? ret : 0;
}

static int tla2528_read_sample(const struct i2c_client *client)
{
	__be16 data;
	int ret;

	ret = i2c_master_recv(client, (char *)&data, 2);
	if (ret < 0)
		return ret;

	return be16_to_cpu(data) >> 4;
}

static int tla2528_read(struct tla2528 *tla2528, u8 channel, int *val)
{
	struct i2c_client *client = tla2528->client;
	int ret;

	if (channel != tla2528->last_read_channel) {
		ret = tla2528_write_reg(client, TLA2528_CHANNEL_SEL_ADR, channel);
		if (ret < 0)
			return ret;

		tla2528->last_read_channel = channel;
	}

	ret = tla2528_read_sample(client);
	if (ret < 0)
		return ret;

	*val = ret;

	return 0;
}

static int tla2528_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct tla2528 *tla2528 = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&tla2528->lock);
		ret = tla2528_read(tla2528, chan->channel, val);
		mutex_unlock(&tla2528->lock);
		if (ret < 0)
			return ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = tla2528->vref_uv / 1000;
		*val2 = 12;

		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

#define TLA2528_CHAN(_chan, _name) { \
	.type = IIO_VOLTAGE,					\
	.channel = (_chan),					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.datasheet_name = _name,				\
	.indexed = 1,						\
}

static const struct iio_chan_spec tla2528_channel[] = {
	TLA2528_CHAN(0, "AIN0"),
	TLA2528_CHAN(1, "AIN1"),
	TLA2528_CHAN(2, "AIN2"),
	TLA2528_CHAN(3, "AIN3"),
	TLA2528_CHAN(4, "AIN4"),
	TLA2528_CHAN(5, "AIN5"),
	TLA2528_CHAN(6, "AIN6"),
	TLA2528_CHAN(7, "AIN7"),
};

static const struct iio_info tla2528_info = {
	.read_raw = tla2528_read_raw,
};

static int tla2528_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct tla2528 *tla2528;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C |
				     I2C_FUNC_SMBUS_WRITE_WORD_DATA))
		return -EOPNOTSUPP;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*tla2528));
	if (!indio_dev)
		return -ENOMEM;

	tla2528 = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	tla2528->client = client;

	indio_dev->name = client->name;
	indio_dev->info = &tla2528_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = tla2528_channel;
	indio_dev->num_channels = ARRAY_SIZE(tla2528_channel);

	mutex_init(&tla2528->lock);

	tla2528->vref_uv = devm_regulator_get_enable_read_voltage(&client->dev,
								  "vref");
	if (tla2528->vref_uv < 0)
		return tla2528->vref_uv;

	/* Set all inputs as analog */
	ret = tla2528_write_reg(tla2528->client, TLA2528_PIN_CFG_ADR, 0x00);
	if (ret < 0)
		return ret;

	ret = tla2528_write_reg(tla2528->client, TLA2528_DATA_CFG_ADR,
				TLA2528_DATA_CFG_APPEND_STATUS);
	if (ret < 0)
		return ret;

	/* Set manual mode */
	ret = tla2528_write_reg(tla2528->client, TLA2528_SEQUENCE_CFG_ADR, 0x00);
	if (ret < 0)
		return ret;

	/* Init private data */
	tla2528->last_read_channel = ~0;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id tla2528_id[] = {
	{ "tla2528", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tla2528_id);

static const struct of_device_id tla2528_of_match[] = {
	{ .compatible = "ti,tla2528", },
	{  },
};
MODULE_DEVICE_TABLE(of, tla2528_of_match);

static struct i2c_driver tla2528_driver = {
	.driver = {
		.name = "tla2528",
		.of_match_table = tla2528_of_match,
	},
	.probe = tla2528_probe,
	.id_table = tla2528_id,
};
module_i2c_driver(tla2528_driver);

MODULE_AUTHOR("Maxime Chevallier <maxime.chevallier@bootlin.com>");
MODULE_AUTHOR("Rodolfo Giometti <giometti@enneenne.com>");
MODULE_DESCRIPTION("Texas Instruments TLA2528 ADC driver");
MODULE_LICENSE("GPL");
