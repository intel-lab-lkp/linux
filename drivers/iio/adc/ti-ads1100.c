// SPDX-License-Identifier: GPL-2.0-only
/*
 * ADS1100 - Texas Instruments Analog-to-Digital Converter
 *
 * Copyright (c) 2023, Topic Embedded Products
 *
 * Datasheet: https://www.ti.com/lit/gpn/ads1100
 * IIO driver for ADS1100, ADS1000 and ADS1110 ADC 16-bit I2C
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>

#include <linux/iio/iio.h>
#include <linux/iio/types.h>

/* The ADS1100 has a single byte config register */

/* Conversion in progress bit */
#define ADS1100_CFG_ST_BSY	BIT(7)
/* Single conversion bit */
#define ADS1100_CFG_SC		BIT(4)
/* Data rate */
#define ADS1100_DR_MASK		GENMASK(3, 2)
/* Gain */
#define ADS1100_PGA_MASK	GENMASK(1, 0)

#define ADS1100_CONTINUOUS	0
#define	ADS1100_SINGLESHOT	ADS1100_CFG_SC

#define ADS1100_SLEEP_DELAY_MS	2000
#define ADS1110_REFERENCE_VOLTAGE_MICROVOLT 2048000

/* Timeout based on the minimum sample rate of 8 SPS (7500000us) */
#define ADS11x0_MAX_DRDY_TIMEOUT	7500000

static const int ads1100_data_rate[] = { 128, 32, 16, 8 };
static const int ads1110_data_rate[] = { 240, 60, 30, 15 };
static const int ads1100_data_rate_bits[] = { 12, 14, 15, 16 };

struct ads11x0_config {
	const int *data_rate;
	bool has_reference_voltage;
};

static const struct ads11x0_config ads1100_config = {
	.data_rate = ads1100_data_rate,
	.has_reference_voltage = false,
};

static const struct ads11x0_config ads1110_config = {
	.data_rate = ads1110_data_rate,
	.has_reference_voltage = true,
};

struct ads1100_data {
	struct i2c_client *client;
	struct regulator *reg_vdd;
	struct mutex lock;
	int scale_avail[2 * 4]; /* 4 gain settings */
	u8 config;
	bool supports_data_rate; /* Only the ADS1100/ADS1110 can select the rate */
	bool has_reference_voltage; /* The ADS1110 has an internal reference, so fixed scale */
	const int *data_rate;
};

static const struct iio_chan_spec ads1100_channel = {
	.type = IIO_VOLTAGE,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	.info_mask_shared_by_all =
	    BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_SAMP_FREQ),
	.info_mask_shared_by_all_available =
	    BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_SAMP_FREQ),
	.datasheet_name = "AIN",
};

static int ads1100_set_config_bits(struct ads1100_data *data, u8 mask, u8 value)
{
	int ret;
	u8 config = (data->config & ~mask) | (value & mask);

	if (data->config == config)
		return 0;	/* Already done */

	ret = i2c_master_send(data->client, &config, 1);
	if (ret < 0)
		return ret;

	data->config = config;

	return 0;
};

static int ads11x0_get_voltage_microvolts(struct ads1100_data *data)
{
	if (data->has_reference_voltage)
		return ADS1110_REFERENCE_VOLTAGE_MICROVOLT;
	else
		return regulator_get_voltage(data->reg_vdd);
}

static int ads11x0_get_voltage_milivolts(struct ads1100_data *data)
{
	return ads11x0_get_voltage_microvolts(data) / (MICRO / MILLI);
}

static bool ads11x0_new_data_ready(struct ads1100_data *data)
{
	int ret;
	u8 buffer[3];

	ret = i2c_master_recv(data->client, (char *)&buffer, sizeof(buffer));
	if (ret < sizeof(buffer)) {
		dev_err(&data->client->dev, "I2C read fail: %d\n", ret);
		return 0;
	}

	int return_val = FIELD_GET(ADS1100_CFG_ST_BSY, buffer[2]);

	return return_val;
}

static int ads11x0_poll_data_ready(struct ads1100_data *data)
{
	bool data_ready;
	u8 buffer[3];
	int datarate = data->data_rate[FIELD_GET(ADS1100_DR_MASK, data->config)];
	unsigned long wait_time = DIV_ROUND_CLOSEST(MICRO, 5 * datarate);

	/* To be sure that polled value will have value after config change */
	i2c_master_recv(data->client, (char *)&buffer, sizeof(buffer));

	return read_poll_timeout(ads11x0_new_data_ready, data_ready,
				 !data_ready, wait_time,
				 ADS11x0_MAX_DRDY_TIMEOUT, false, data);
}

static int ads1100_data_bits(struct ads1100_data *data)
{
	return ads1100_data_rate_bits[FIELD_GET(ADS1100_DR_MASK, data->config)];
}

static int ads1100_get_adc_result(struct ads1100_data *data, int chan, int *val)
{
	int ret;
	__be16 buffer;
	s16 value;

	if (chan != 0)
		return -EINVAL;

	ret = pm_runtime_resume_and_get(&data->client->dev);
	if (ret < 0)
		return ret;

	ret = i2c_master_recv(data->client, (char *)&buffer, sizeof(buffer));

	pm_runtime_mark_last_busy(&data->client->dev);
	pm_runtime_put_autosuspend(&data->client->dev);

	if (ret < sizeof(buffer)) {
		dev_err(&data->client->dev, "I2C read fail: %d\n", ret);
		return ret;
	}

	/* Value is always 16-bit 2's complement */
	value = be16_to_cpu(buffer);

	/* Shift result to compensate for bit resolution vs. sample rate */
	value <<= 16 - ads1100_data_bits(data);

	*val = sign_extend32(value, 15);

	return 0;
}

static int ads1100_set_scale(struct ads1100_data *data, int val, int val2)
{
	int microvolts;
	int gain;
	int ret;
	/* With Vdd between 2.7 and 5V, the scale is always below 1 */
	if (val)
		return -EINVAL;

	if (!val2)
		return -EINVAL;

	microvolts = ads11x0_get_voltage_microvolts(data);
	/*
	 * val2 is in 'micro' units, n = val2 / 1000000
	 * result must be millivolts, d = microvolts / 1000
	 * the full-scale value is d/n, corresponds to 2^15,
	 * hence the gain = (d / n) >> 15, factoring out the 1000 and moving the
	 * bitshift so everything fits in 32-bits yields this formula.
	 */
	gain = DIV_ROUND_CLOSEST(microvolts, BIT(15)) * MILLI / val2;
	if (gain < BIT(0) || gain > BIT(3))
		return -EINVAL;

	ret = pm_runtime_resume_and_get(&data->client->dev);
	if (ret < 0)
		return ret;

	ads1100_set_config_bits(data, ADS1100_PGA_MASK, ffs(gain) - 1);

	ret = ads11x0_poll_data_ready(data);

	pm_runtime_mark_last_busy(&data->client->dev);
	pm_runtime_put_autosuspend(&data->client->dev);

	return ret;
}

static int ads1100_set_data_rate(struct ads1100_data *data, int chan, int rate)
{
	unsigned int i;
	unsigned int size;
	int ret;

	size = data->supports_data_rate ? ARRAY_SIZE(ads1100_data_rate) : 1;
	for (i = 0; i < size; i++) {
		if (data->data_rate[i] == rate) {
			ret = pm_runtime_resume_and_get(&data->client->dev);
			if (ret < 0)
				return ret;

			ads1100_set_config_bits(data, ADS1100_DR_MASK,
						FIELD_PREP(ADS1100_DR_MASK, i));
			ret = ads11x0_poll_data_ready(data);

			pm_runtime_mark_last_busy(&data->client->dev);
			pm_runtime_put_autosuspend(&data->client->dev);
			return ret;
		}
	}

	return -EINVAL;
}

static void ads1100_calc_scale_avail(struct ads1100_data *data)
{
	int millivolts = ads11x0_get_voltage_milivolts(data);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(data->scale_avail) / 2; i++) {
		data->scale_avail[i * 2 + 0] = millivolts;
		data->scale_avail[i * 2 + 1] = 15 + i;
	}
}

static int ads1100_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	struct ads1100_data *data = iio_priv(indio_dev);

	if (chan->type != IIO_VOLTAGE)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT;
		*vals = data->data_rate;
		if (data->supports_data_rate)
			*length = ARRAY_SIZE(ads1100_data_rate);
		else
			*length = 1;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_SCALE:
		*type = IIO_VAL_FRACTIONAL_LOG2;
		*vals = data->scale_avail;
		*length = ARRAY_SIZE(data->scale_avail);
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int ads1100_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	int ret;
	struct ads1100_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->lock);
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;

		ret = ads1100_get_adc_result(data, chan->address, val);
		iio_device_release_direct(indio_dev);
		if (ret < 0)
			return ret;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/* full-scale is the supply voltage in millivolts */
		*val = ads11x0_get_voltage_milivolts(data);
		*val2 = 15 + FIELD_GET(ADS1100_PGA_MASK, data->config);
		return IIO_VAL_FRACTIONAL_LOG2;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = data->data_rate[FIELD_GET(ADS1100_DR_MASK, data->config)];
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ads1100_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct ads1100_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->lock);
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return ads1100_set_scale(data, val, val2);
	case IIO_CHAN_INFO_SAMP_FREQ:
		return ads1100_set_data_rate(data, chan->address, val);
	default:
		return -EINVAL;
	}
}

static const struct iio_info ads1100_info = {
	.read_avail = ads1100_read_avail,
	.read_raw = ads1100_read_raw,
	.write_raw = ads1100_write_raw,
};

static int ads1100_setup(struct ads1100_data *data)
{
	int ret;
	u8 buffer[3];

	/* Setup continuous sampling mode at 8sps */
	buffer[0] = ADS1100_DR_MASK | ADS1100_CONTINUOUS;
	ret = i2c_master_send(data->client, buffer, 1);
	if (ret < 0)
		return ret;

	ret = i2c_master_recv(data->client, buffer, sizeof(buffer));
	if (ret < sizeof(buffer))
		return -1;

	/* Config register returned in third byte, strip away the busy status */
	data->config = buffer[2] & ~ADS1100_CFG_ST_BSY;

	/* Detect the sample rate capability by checking the DR bits */
	data->supports_data_rate = FIELD_GET(ADS1100_DR_MASK, buffer[2]) != 0;

	return 0;
}

static void ads1100_reg_disable(void *data)
{
	struct ads1100_data *ads1100_data = data;

	/* Disable when not already disabled by the driver */
	if (!(ads1100_data->config & ADS1100_CFG_SC))
		regulator_disable(ads1100_data->reg_vdd);
}

static void ads1100_disable_continuous(void *data)
{
	ads1100_set_config_bits(data, ADS1100_CFG_SC, ADS1100_SINGLESHOT);
}

static int ads1100_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct ads1100_data *data;
	struct device *dev = &client->dev;
	const struct ads11x0_config *model;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	dev_set_drvdata(dev, data);
	data->client = client;
	mutex_init(&data->lock);

	indio_dev->name = "ads1100";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = &ads1100_channel;
	indio_dev->num_channels = 1;
	indio_dev->info = &ads1100_info;

	data->reg_vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(data->reg_vdd))
		return dev_err_probe(dev, PTR_ERR(data->reg_vdd),
				     "Failed to get vdd regulator\n");

	ret = regulator_enable(data->reg_vdd);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "Failed to enable vdd regulator\n");

	ret = devm_add_action_or_reset(dev, ads1100_reg_disable, data);
	if (ret)
		return ret;

	model = device_get_match_data(dev);
	if (!model)
		return dev_err_probe(dev, ret,
				"Can't set device data\n");

	data->data_rate = model->data_rate;
	data->has_reference_voltage = model->has_reference_voltage;

	ret = ads1100_setup(data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to communicate with device\n");

	ret = devm_add_action_or_reset(dev, ads1100_disable_continuous, data);
	if (ret)
		return ret;

	ads1100_calc_scale_avail(data);

	pm_runtime_set_autosuspend_delay(dev, ADS1100_SLEEP_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable pm_runtime\n");

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register IIO device\n");

	return 0;
}

static int ads1100_runtime_suspend(struct device *dev)
{
	struct ads1100_data *data = dev_get_drvdata(dev);

	ads1100_set_config_bits(data, ADS1100_CFG_SC, ADS1100_SINGLESHOT);
	regulator_disable(data->reg_vdd);

	return 0;
}

static int ads1100_runtime_resume(struct device *dev)
{
	struct ads1100_data *data = dev_get_drvdata(dev);
	int ret;

	ret = regulator_enable(data->reg_vdd);
	if (ret) {
		dev_err(&data->client->dev, "Failed to enable Vdd\n");
		return ret;
	}

	/*
	 * We'll always change the mode bit in the config register, so there is
	 * no need here to "force" a write to the config register. If the device
	 * has been power-cycled, we'll re-write its config register now.
	 */
	return ads1100_set_config_bits(data, ADS1100_CFG_SC,
				       ADS1100_CONTINUOUS);
}

static DEFINE_RUNTIME_DEV_PM_OPS(ads1100_pm_ops,
				 ads1100_runtime_suspend,
				 ads1100_runtime_resume,
				 NULL);

static const struct i2c_device_id ads1100_id[] = {
	{ .name = "ads1100", .driver_data = (kernel_ulong_t)&ads1100_config },
	{ .name = "ads1000", .driver_data = (kernel_ulong_t)&ads1100_config },
	{ .name = "ads1110", .driver_data = (kernel_ulong_t)&ads1110_config },
	{ }
};

MODULE_DEVICE_TABLE(i2c, ads1100_id);

static const struct of_device_id ads1100_of_match[] = {
	{ .compatible = "ti,ads1100", .data = &ads1100_config },
	{ .compatible = "ti,ads1000", .data = &ads1100_config },
	{ .compatible = "ti,ads1110", .data = &ads1110_config },
	{ }
};

MODULE_DEVICE_TABLE(of, ads1100_of_match);

static struct i2c_driver ads1100_driver = {
	.driver = {
		   .name = "ads1100",
		   .of_match_table = ads1100_of_match,
		   .pm = pm_ptr(&ads1100_pm_ops),
	},
	.probe = ads1100_probe,
	.id_table = ads1100_id,
};

module_i2c_driver(ads1100_driver);

MODULE_AUTHOR("Mike Looijmans <mike.looijmans@topic.nl>");
MODULE_DESCRIPTION("Texas Instruments ADS1100 ADC driver");
MODULE_LICENSE("GPL");
