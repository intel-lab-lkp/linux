// SPDX-License-Identifier: GPL-2.0
/*
 * Texas Instruments ADS112C04 16-bit I2C ADC driver
 * Based on TI Reference Code and standard Linux IIO framework.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/regulator/consumer.h>

/* ADS112C04 Commands */
#define ADS112C04_CMD_RESET         0x06
#define ADS112C04_CMD_START_SYNC    0x08
#define ADS112C04_CMD_POWERDOWN     0x02
#define ADS112C04_CMD_RDATA         0x10
#define ADS112C04_CMD_RREG(reg)     (0x20 | ((reg) << 2))
#define ADS112C04_CMD_WREG(reg)     (0x40 | ((reg) << 2))

/* Registers */
#define ADS112C04_REG_CONFIG0       0x00
#define ADS112C04_REG_CONFIG1       0x01
#define ADS112C04_REG_CONFIG2       0x02
#define ADS112C04_REG_CONFIG3       0x03

/* Config2 Masks */
#define ADS112C04_DRDY_MASK         BIT(7)

struct ads112c04_state {
	struct i2c_client *client;
	/* Protects concurrent ADC reads and device configuration */
	struct mutex lock;
	struct completion completion;
	struct regulator *vref_reg;
	int vref_mv;
	u8 config0;
	u8 config1;
};

static int ads112c04_write_cmd(struct i2c_client *client, u8 cmd)
{
	return i2c_master_send(client, &cmd, 1);
}

static int ads112c04_read_reg(struct i2c_client *client, u8 reg, u8 *val)
{
	u8 cmd = ADS112C04_CMD_RREG(reg);
	int ret;

	ret = i2c_master_send(client, &cmd, 1);
	if (ret < 0)
		return ret;

	ret = i2c_master_recv(client, val, 1);
	return ret < 0 ? ret : 0;
}

static int ads112c04_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { ADS112C04_CMD_WREG(reg), val };
	int ret;

	ret = i2c_master_send(client, buf, 2);
	return ret < 0 ? ret : 0;
}

static int ads112c04_wait_for_data(struct ads112c04_state *st)
{
	int ret, timeout = 50;
	u8 val;

	/* If IRQ is available, wait for DRDY interrupt completion */
	if (st->client->irq > 0) {
		ret = wait_for_completion_timeout(&st->completion, msecs_to_jiffies(1000));
		if (!ret)
			return -ETIMEDOUT;
		return 0;
	}

	/* Fallback: Poll DRDY bit in CONFIG2 */
	while (timeout--) {
		ret = ads112c04_read_reg(st->client, ADS112C04_REG_CONFIG2, &val);
		if (ret < 0)
			return ret;
		if (val & ADS112C04_DRDY_MASK)
			return 0;
		usleep_range(1000, 2000);
	}

	return -ETIMEDOUT;
}

static int ads112c04_read_data(struct ads112c04_state *st, int *val)
{
	u8 cmd = ADS112C04_CMD_RDATA;
	u8 buf[2];
	int ret;

	ret = i2c_master_send(st->client, &cmd, 1);
	if (ret < 0)
		return ret;

	ret = i2c_master_recv(st->client, buf, 2);
	if (ret < 0)
		return ret;

	/* 16-bit 2's complement conversion */
	*val = (s16)((buf[0] << 8) | buf[1]);
	return 0;
}

static int ads112c04_get_adc_result(struct ads112c04_state *st,
				    struct iio_chan_spec const *chan,
				    int *val)
{
	int ret;
	u8 mux;

	mux = (chan->address << 4) & 0xF0;
	if ((st->config0 & 0xF0) != mux) {
		st->config0 = (st->config0 & 0x0F) | mux;
		ads112c04_write_reg(st->client, ADS112C04_REG_CONFIG0, st->config0);
	}

	reinit_completion(&st->completion);

	ret = ads112c04_write_cmd(st->client, ADS112C04_CMD_START_SYNC);
	if (ret < 0)
		return ret;

	ret = ads112c04_wait_for_data(st);
	if (ret < 0)
		return ret;

	return ads112c04_read_data(st, val);
}

static int ads112c04_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct ads112c04_state *st = iio_priv(indio_dev);
	int ret, raw;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&st->lock);
		ret = ads112c04_get_adc_result(st, chan, val);
		mutex_unlock(&st->lock);

		if (ret < 0)
			return ret;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = st->vref_mv;
		*val2 = 15; /* 2^15 */
		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static irqreturn_t ads112c04_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct ads112c04_state *st = iio_priv(indio_dev);

	complete(&st->completion);
	return IRQ_HANDLED;
}

#define ADS112C04_V_CHAN(_chan, _addr) {			\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = _chan,					\
	.address = _addr,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
}

#define ADS112C04_V_DIFF_CHAN(_chan, _chan2, _addr) {		\
	.type = IIO_VOLTAGE,					\
	.differential = 1,					\
	.indexed = 1,						\
	.channel = _chan,					\
	.channel2 = _chan2,					\
	.address = _addr,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
}

static const struct iio_chan_spec ads112c04_channels[] = {
	ADS112C04_V_DIFF_CHAN(0, 1, 0x00),
	ADS112C04_V_DIFF_CHAN(0, 2, 0x01),
	ADS112C04_V_DIFF_CHAN(0, 3, 0x02),
	ADS112C04_V_DIFF_CHAN(1, 0, 0x03),
	ADS112C04_V_DIFF_CHAN(1, 2, 0x04),
	ADS112C04_V_DIFF_CHAN(1, 3, 0x05),
	ADS112C04_V_DIFF_CHAN(2, 3, 0x06),
	ADS112C04_V_DIFF_CHAN(3, 2, 0x07),
	ADS112C04_V_CHAN(0, 0x08),
	ADS112C04_V_CHAN(1, 0x09),
	ADS112C04_V_CHAN(2, 0x0A),
	ADS112C04_V_CHAN(3, 0x0B),
};

static const struct iio_info ads112c04_info = {
	.read_raw = ads112c04_read_raw,
};

static void ads112c04_regulator_disable(void *data)
{
	regulator_disable(data);
}

static int ads112c04_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct ads112c04_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->client = client;
	mutex_init(&st->lock);
	init_completion(&st->completion);

	indio_dev->name = "ads112c04";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ads112c04_info;
	indio_dev->channels = ads112c04_channels;
	indio_dev->num_channels = ARRAY_SIZE(ads112c04_channels);

	st->vref_reg = devm_regulator_get_optional(&client->dev, "vref");
	if (IS_ERR(st->vref_reg)) {
		ret = PTR_ERR(st->vref_reg);
		if (ret == -ENODEV) {
			st->vref_mv = 2048;
			st->config1 = 0x00; /* VREF[1:0] = 00 */
		} else {
			return ret;
		}
	} else {
		ret = regulator_enable(st->vref_reg);
		if (ret)
			return ret;

		ret = devm_add_action_or_reset(&client->dev, ads112c04_regulator_disable,
					       st->vref_reg);
		if (ret)
			return ret;

		ret = regulator_get_voltage(st->vref_reg);
		if (ret < 0)
			return ret;

		st->vref_mv = ret / 1000;
		st->config1 = 0x02;
	}

	/* Reset device to defaults */
	ads112c04_write_cmd(client, ADS112C04_CMD_RESET);
	usleep_range(1000, 2000);

	/* Set defaults: PGA Disabled (For single-ended safety), Gain=1 */
	st->config0 = 0x01;

	ads112c04_write_reg(client, ADS112C04_REG_CONFIG1, st->config1);

	if (client->irq > 0) {
		ret = devm_request_irq(&client->dev, client->irq,
				       ads112c04_irq_handler,
				       IRQF_TRIGGER_FALLING,
				       indio_dev->name, indio_dev);
		if (ret) {
			dev_err(&client->dev, "Failed to request DRDY IRQ\n");
			return ret;
		}
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id ads112c04_id[] = {
	{ "ads112c04", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ads112c04_id);

static const struct of_device_id ads112c04_of_match[] = {
	{ .compatible = "ti,ads112c04" },
	{ }
};
MODULE_DEVICE_TABLE(of, ads112c04_of_match);

static struct i2c_driver ads112c04_driver = {
	.driver = {
		.name = "ads112c04",
		.of_match_table = ads112c04_of_match,
	},
	.probe = ads112c04_probe,
	.id_table = ads112c04_id,
};
module_i2c_driver(ads112c04_driver);

MODULE_AUTHOR("Kyle Hsieh <kylehsieh1995@gmail.com>");
MODULE_DESCRIPTION("Texas Instruments ADS112C04 ADC driver");
MODULE_LICENSE("GPL");
