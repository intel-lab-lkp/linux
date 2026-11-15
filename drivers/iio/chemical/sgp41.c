// SPDX-License-Identifier: GPL-2.0
/*
 * sgp41 - Support for Sensirion SGP41 Gas Sensor
 *
 * Copyright (C) 2026 Akshat Chandra <notmissinge@gmail.com>
 *
 * I2C slave address: 0x59
 *
 * Datasheet can be found here:
 * https://www.sensirion.com/file/datasheet_sgp41
 *
 * There are two functionalities supported:
 *
 * 1) read raw logarithmic resistance value from sensor
 *    --> useful to pass it to the algorithm of the sensor vendor for
 *    measuring deteriorations and improvements of air quality.
 *    The values can be read from the in_resistance_voc_raw and
 *    in_resistance_nox_raw.
 *
 * 2) calculate an estimated VOC index (0 - 500) and expose it through
 *    the in_concentration_voc_input attribute.
 *
 *    The VOC index is calculated using the raw VOC resistance and a
 *    configurable calibration bias, exposed through
 *    in_resistance_voc_calibbias (default 30000).
 *
 * The driver does not calculate a NOx index.
 *
 * Conditioning runs before the first raw measurement, once per second
 * for a total of 10 seconds.
 *
 * Compensation values for relative humidity and temperature can be
 * configured through the corresponding output attributes.
 *
 * The sensor heater is turned off when the device is removed.
 */

#include<linux/cleanup.h>
#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>

#define SGP41_CALC_POWER   14
#define SGP41_CRC8_POLYNOMIAL		0x31
#define SGP41_CRC8_INIT		0xff

#define SGP41_COND_ITERATIONS		10
#define SRAW_VOC           1
#define SRAW_NOX           2

DECLARE_CRC8_TABLE(sgp41_crc8_table);

struct sgp41_data {
	struct device *dev;
	struct i2c_client *client;

	int rht;
	int temp;

	int voc_calibbias;

	struct mutex lock; /* Protects sensor state and I2C transaction.*/
	bool conditioned;
	u8 condition_count; /*Counter for conditioning_cmd sent.*/
};

struct sgp41_tg_measure {
	u8 command[2];
	__be16 rht_ticks;
	u8 rht_crc;
	__be16 temp_ticks;
	u8 temp_crc;
} __packed;

struct sgp41_tg_result {
	__be16 voc_res_ticks;
	u8 voc_res_crc;
	__be16 nox_res_ticks;
	u8 nox_res_crc;
} __packed;

static const struct iio_chan_spec sgp41_channels[] = {
	{
		.type = IIO_RESISTANCE,/*VOC*/
		.channel = SRAW_VOC,
		.extend_name = "voc",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_CALIBBIAS),
	},
	{
		.type = IIO_RESISTANCE,/*NOX*/
		.channel = SRAW_NOX,
		.extend_name = "nox",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_CONCENTRATION,
		.channel2 = IIO_MOD_VOC,
		.modified = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
	{
		.type = IIO_TEMP,
		.output = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_HUMIDITYRELATIVE,
		.output = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
};

/*
 * taylor approximation of e^x:
 * y = 1 + x + x^2 / 2 + x^3 / 6 + x^4 / 24 + ... + x^n / n!
 *
 * Because we are calculating x real value multiplied by 2^power we get
 * an additional 2^power^n to divide for every element. For a reasonable
 * precision this would overflow after a few iterations. Therefore we
 * divide the x^n part whenever its about to overflow (xmax).
 */

static u32 sgp41_exp(int exp, u32 power, u32 rounds)
{
	u32 x, y, xp;
	u32 factorial, divider, xmax;
	int sign = 1;
	u32 i;

	if (exp == 0) {
		return 1 << power;
	} else if (exp < 0) {
		sign = -1;
		exp *= -1;
	}

	xmax = 0x7FFFFFFF / exp;
	x = exp;
	xp = 1;
	factorial = 1;
	y = 1 << power;
	divider = 0;

	for (i = 1; i <= rounds; i++) {
		xp *= x;
		factorial *= i;
		y += (xp >> divider) / factorial;
		divider += power;
		/* divide when next multiplication would overflow */
		if (xp >= xmax) {
			xp >>= power;
			divider -= power;
		}
	}

	if (sign == -1)
		return (1 << (power * 2)) / y;
	else
		return y;
}

static void temp_rht_payload(struct sgp41_data *data,
			     struct sgp41_tg_measure *tg)
{
	s64 ticks;
	u16 ticks16;

	ticks = div_s64((s64)data->rht * 65535, 100000);
	ticks16 = (u16)clamp_t(s64, ticks, 0, 65535);
	tg->rht_ticks = cpu_to_be16(ticks16);
	tg->rht_crc = crc8(sgp41_crc8_table, (u8 *)&tg->rht_ticks, 2,
			   SGP41_CRC8_INIT);

	ticks = div_s64((s64)(data->temp + 45000) * 65535, 175000);
	ticks16 = (u16)clamp_t(s64, ticks, 0, 65535);
	tg->temp_ticks = cpu_to_be16(ticks16);
	tg->temp_crc = crc8(sgp41_crc8_table, (u8 *)&tg->temp_ticks, 2,
			    SGP41_CRC8_INIT);
}

static int sgp41_calc_voc(struct sgp41_data *data, u16 voc_raw, int *voc)
{
	int x;
	u32 exp = 0;
	int voc_calibbias;

	/* we calculate as a multiple of 16384 (2^14) */
	scoped_guard(mutex, &data->lock)
		voc_calibbias = data->voc_calibbias;

	x = ((int)voc_raw - voc_calibbias) * 106;
		/* voc = 500 / (1 + e^x) */
	exp = sgp41_exp(x, SGP41_CALC_POWER, 18);
	*voc = 500 * ((1 << (SGP41_CALC_POWER * 2)) / ((1 << SGP41_CALC_POWER) + exp));

	dev_dbg(data->dev, "voc_raw: %d voc_calibbias: %d x: %d exp: %u voc: %d\n",
		voc_raw, data->voc_calibbias, x, exp, *voc);

	return 0;
}

static int sgp41_run_conditioning(struct sgp41_data *data)
{
	struct sgp41_tg_measure tg = {
		.command = { 0x26, 0x12 },
	};
	int i;
	int ret;

	for (i = 0; i < SGP41_COND_ITERATIONS; i++) {
		if (data->condition_count!= i)
			i = data->condition_count;

		temp_rht_payload(data, &tg);
		ret = i2c_master_send(data->client, (const char *)&tg,
				      sizeof(tg));
		if (ret != sizeof(tg)) {
			dev_warn(data->dev,
				 "i2c_master_send ret: %d sizeof: %zu\n",
				 ret, sizeof(tg));
			return ret < 0 ? ret : -EIO;
		}
		data->condition_count++;
		msleep(50);

		if (i != SGP41_COND_ITERATIONS - 1)
			msleep(950);
	}

	data->conditioned = true;

	return 0;
}

static int sgp41_heater_off(struct sgp41_data *data)
{
	u8 heater_off_cmd[2] = { 0x36, 0x15 };
	int ret;

	ret = i2c_master_send(data->client, heater_off_cmd,
			      sizeof(heater_off_cmd));
	if (ret != sizeof(heater_off_cmd)) {
		dev_warn(data->dev,
			 "i2c_master_send ret: %d sizeof: %zu\n",
			 ret, sizeof(heater_off_cmd));
		return ret < 0 ? ret : -EIO;
	}
	data-> conditioned = false;
	data->condition_count = 0;
	dev_dbg(data->dev, "heater turned off\n");

	return 0;
}

static int sgp41_measure_raw(struct sgp41_data *data,
			     u16 *voc_raw, u16 *nox_raw)
{
	struct sgp41_tg_measure tg = {
		.command = { 0x26, 0x19 },
	};
	struct sgp41_tg_result tgres;
	u8 crc;
	int ret;

	guard(mutex)(&data->lock);

	if (!data->conditioned) {
		ret = sgp41_run_conditioning(data);
		if (ret) {
			sgp41_heater_off(data);
			return ret;
		}
	}

	temp_rht_payload(data, &tg);

	ret = i2c_master_send(data->client, (const char *)&tg, sizeof(tg));
	if (ret != sizeof(tg)) {
		dev_warn(data->dev,
			 "i2c_master_send ret: %d sizeof: %zu\n",
			 ret, sizeof(tg));
		return ret < 0 ? ret : -EIO;
	}

	msleep(50);

	ret = i2c_master_recv(data->client, (u8 *)&tgres, sizeof(tgres));
	if (ret < 0)
		return ret;

	if (ret != sizeof(tgres)) {
		dev_warn(data->dev,
			 "i2c_master_recv ret: %d sizeof: %zu\n",
			 ret, sizeof(tgres));
		return -EIO;
	}

	crc = crc8(sgp41_crc8_table, (u8 *)&tgres.voc_res_ticks, 2,
		   SGP41_CRC8_INIT);
	if (crc != tgres.voc_res_crc) {
		dev_err(data->dev, "CRC error while measure-voc_raw\n");
		return -EIO;
	}

	crc = crc8(sgp41_crc8_table, (u8 *)&tgres.nox_res_ticks, 2,
		   SGP41_CRC8_INIT);
	if (crc != tgres.nox_res_crc) {
		dev_err(data->dev, "CRC error while measure-nox_raw\n");
		return -EIO;
	}

	*voc_raw = be16_to_cpu(tgres.voc_res_ticks);
	*nox_raw = be16_to_cpu(tgres.nox_res_ticks);

	return 0;
}

static int sgp41_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan, int *val,
			  int *val2, long mask)
{
	struct sgp41_data *data = iio_priv(indio_dev);
	u16 voc_raw;
	u16 nox_raw;
	int ret;
	int voc;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_RESISTANCE: {
			ret = sgp41_measure_raw(data, &voc_raw, &nox_raw);
			if (ret)
				return ret;

			switch (chan->channel) {
			case SRAW_VOC:
				*val = voc_raw;
				return IIO_VAL_INT;
			case SRAW_NOX:
				*val = nox_raw;
				return IIO_VAL_INT;
			default:
				return -EINVAL;
			}
		}
		case IIO_TEMP: {
			guard(mutex)(&data->lock);
			*val = data->temp;
			return IIO_VAL_INT;
		}
		case IIO_HUMIDITYRELATIVE: {
			guard(mutex)(&data->lock);
			*val = data->rht;
			return IIO_VAL_INT;
		}
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_PROCESSED:
		switch (chan->channel2) {
		case IIO_MOD_VOC:
			ret = sgp41_measure_raw(data, &voc_raw, &nox_raw);
			if (ret)
				return ret;

			ret = sgp41_calc_voc(data, voc_raw, &voc);
			if (ret)
				return ret;

			*val = voc / (1 << SGP41_CALC_POWER);
		/*
		 * calculation should fit into integer, where:
		 * voc <= (500 * 2^SGP41_CALC_POWER) = 8192000
		 * (with SGP41_CALC_POWER = 14)
		 */
		*val2 = ((voc % (1 << SGP41_CALC_POWER)) * 244) /
			(1 << (SGP41_CALC_POWER - 12));
		dev_dbg(data->dev, "voc: %d val: %d.%06d\n",
			voc, *val, *val2);
		return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_CALIBBIAS:
		switch (chan->channel) {
		/*For VOC Calibration,*/
		case SRAW_VOC: {
			guard(mutex)(&data->lock);
			*val = data->voc_calibbias;
			return IIO_VAL_INT;
		}
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int sgp41_write_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan, int val,
			   int val2, long mask)
{
	struct sgp41_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_TEMP: {
			if (val < -45000 || val > 130000)
				return -EINVAL;

			guard(mutex)(&data->lock);
			data->temp = val;
			return 0;
		}
		case IIO_HUMIDITYRELATIVE: {
			if (val < 0 || val > 100000)
				return -EINVAL;

			guard(mutex)(&data->lock);
			data->rht = val;
			return 0;
		}
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_CALIBBIAS:
		switch (chan->channel) {
		/*For VOC Calibration,*/
		case SRAW_VOC: {
			if (val < 20000 || val > 52768)
				return -EINVAL;

			guard(mutex)(&data->lock);
			data->voc_calibbias = val;
			return 0;
		}
		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}


static void sgp41_remove(struct i2c_client *client)
{
	int ret;
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct sgp41_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);

	guard(mutex)(&data->lock);
	ret = sgp41_heater_off(data);
	if (ret)
	dev_warn(data->dev, "Failed to turn off heater: %d\n", ret);
}

static const struct iio_info sgp41_info = {
	.read_raw = sgp41_read_raw,
	.write_raw = sgp41_write_raw,
};

static int sgp41_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct sgp41_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	i2c_set_clientdata(client, indio_dev);

	data = iio_priv(indio_dev);
	data->client = client;
	data->dev = dev;
	data->conditioned = false;
	data->condition_count = 0;

	crc8_populate_msb(sgp41_crc8_table, SGP41_CRC8_POLYNOMIAL);

	mutex_init(&data->lock);

	/*default values.*/
	data->rht = 50000;
	data->temp = 25000;
	data->voc_calibbias = 30000; /*resistance raw value for voc index of 250*/

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to get/enable vdd supply\n");

	indio_dev->info = &sgp41_info;

	if (id)
		indio_dev->name = id->name;
	else
		indio_dev->name = "sgp41";

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = sgp41_channels;
	indio_dev->num_channels = ARRAY_SIZE(sgp41_channels);

	ret = iio_device_register(indio_dev);
	if (ret)
		dev_err(dev, "failed to register iio device\n");

	return ret;
}

static const struct i2c_device_id sgp41_id[] = {
	{ .name = "sgp41" },
	{ }
};

MODULE_DEVICE_TABLE(i2c, sgp41_id);

static const struct of_device_id sgp41_dt_ids[] = {
	{ .compatible = "sensirion,sgp41" },
	{ }
};

MODULE_DEVICE_TABLE(of, sgp41_dt_ids);

