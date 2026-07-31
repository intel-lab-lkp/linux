// SPDX-License-Identifier: GPL-2.0-only
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
#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/property.h>
#include <linux/gpio/consumer.h>

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

#define ADS112C04_DRDY_MASK         BIT(7)
#define ADS112C04_MUX_MASK          GENMASK(7, 4)

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
	int ret = i2c_master_send(client, &cmd, 1);

	return ret < 0 ? ret : 0;
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
	int ret;
	u8 val;

	if (st->client->irq > 0) {
		ret = wait_for_completion_timeout(&st->completion, msecs_to_jiffies(1000));
		if (!ret)
			return -ETIMEDOUT;
		return 0;
	}

	return read_poll_timeout(ads112c04_read_reg, ret,
				 (ret < 0 || (val & ADS112C04_DRDY_MASK)),
				 1000, 1000000, false,
				 st->client, ADS112C04_REG_CONFIG2, &val);
}

static int ads112c04_read_data(struct ads112c04_state *st, int *val)
{
	u8 cmd = ADS112C04_CMD_RDATA;
	__be16 buf;
	int ret;

	ret = i2c_master_send(st->client, &cmd, 1);
	if (ret < 0)
		return ret;

	ret = i2c_master_recv(st->client, (u8 *)&buf, 2);
	if (ret < 0)
		return ret;

	*val = sign_extend32(be16_to_cpu(buf), 15);
	return 0;
}

static int ads112c04_get_adc_result(struct ads112c04_state *st,
				    struct iio_chan_spec const *chan,
				    int *val)
{
	int ret;
	u8 mux, new_config0;

	mux = FIELD_PREP(ADS112C04_MUX_MASK, chan->address);
	new_config0 = (st->config0 & 0x0F) | mux;

	if (st->config0 != new_config0) {
		ret = ads112c04_write_reg(st->client, ADS112C04_REG_CONFIG0, new_config0);
		if (ret < 0)
			return ret;
		st->config0 = new_config0;
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
	int ret;

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
		*val2 = 15;
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

static const struct iio_info ads112c04_info = {
	.read_raw = ads112c04_read_raw,
};

static void ads112c04_regulator_disable(void *data)
{
	regulator_disable(data);
}

static int ads112c04_parse_channels(struct iio_dev *indio_dev)
{
	struct device *dev = indio_dev->dev.parent;
	struct iio_chan_spec *channels;
	u32 num_channels, i = 0, pair[2];

	num_channels = device_get_named_child_node_count(dev, "channel");
	if (!num_channels)
		return -EINVAL;

	channels = devm_kcalloc(dev, num_channels, sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	device_for_each_named_child_node_scoped(dev, child, "channel") {
		struct iio_chan_spec *spec = &channels[i];

		spec->type = IIO_VOLTAGE;
		spec->indexed = 1;
		spec->info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE);
		spec->scan_index = i;

		if (fwnode_property_present(child, "single-channel")) {
			fwnode_property_read_u32(child, "single-channel", &pair[0]);
			spec->channel = pair[0];
			spec->differential = 0;
			spec->address = 0x08 + pair[0];
		} else if (fwnode_property_present(child, "diff-channels")) {
			fwnode_property_read_u32_array(child, "diff-channels", pair, 2);
			spec->channel = pair[0];
			spec->channel2 = pair[1];
			spec->differential = 1;

			if (pair[0] == 0 && pair[1] == 1)
				spec->address = 0x00;
			else if (pair[0] == 0 && pair[1] == 2)
				spec->address = 0x01;
			else if (pair[0] == 0 && pair[1] == 3)
				spec->address = 0x02;
			else if (pair[0] == 1 && pair[1] == 0)
				spec->address = 0x03;
			else if (pair[0] == 1 && pair[1] == 2)
				spec->address = 0x04;
			else if (pair[0] == 1 && pair[1] == 3)
				spec->address = 0x05;
			else if (pair[0] == 2 && pair[1] == 3)
				spec->address = 0x06;
			else if (pair[0] == 3 && pair[1] == 2)
				spec->address = 0x07;
			else
				return -EINVAL;
		} else {
			return -EINVAL;
		}
		i++;
	}

	indio_dev->channels = channels;
	indio_dev->num_channels = num_channels;

	return 0;
}

static int ads112c04_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct ads112c04_state *st;
	struct gpio_desc *reset_gpio;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->client = client;

	ret = devm_mutex_init(&client->dev, &st->lock);
	if (ret)
		return ret;

	init_completion(&st->completion);

	indio_dev->name = "ads112c04";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ads112c04_info;

	ret = ads112c04_parse_channels(indio_dev);
	if (ret)
		return ret;

	ret = devm_regulator_get_enable(&client->dev, "avdd");
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to get avdd regulator\n");

	ret = devm_regulator_get_enable(&client->dev, "dvdd");
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to get dvdd regulator\n");

	st->vref_reg = devm_regulator_get_optional(&client->dev, "refp");
	if (IS_ERR(st->vref_reg)) {
		ret = PTR_ERR(st->vref_reg);
		if (ret == -ENODEV) {
			st->vref_mv = 2048;
			st->config1 = 0x00;
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

	reset_gpio = devm_gpiod_get_optional(&client->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio))
		return PTR_ERR(reset_gpio);

	if (reset_gpio) {
		gpiod_set_value_cansleep(reset_gpio, 1);
		fsleep(1000);
		gpiod_set_value_cansleep(reset_gpio, 0);
	} else {
		ret = ads112c04_write_cmd(client, ADS112C04_CMD_RESET);
		if (ret < 0)
			return ret;
	}

	fsleep(1000);

	st->config0 = 0x01;
	ret = ads112c04_write_reg(client, ADS112C04_REG_CONFIG0, st->config0);
	if (ret)
		return ret;

	ret = ads112c04_write_reg(client, ADS112C04_REG_CONFIG1, st->config1);
	if (ret)
		return ret;

	if (client->irq > 0) {
		ret = devm_request_irq(&client->dev, client->irq,
				       ads112c04_irq_handler,
				       0,
				       indio_dev->name, indio_dev);
		if (ret) {
			dev_err(&client->dev, "Failed to request DRDY IRQ\n");
			return ret;
		}
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id ads112c04_id[] = {
	{ .name = "ads112c04", .driver_data = 0 },
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
