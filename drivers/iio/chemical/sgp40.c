// SPDX-License-Identifier: GPL-2.0+
/*
 * sgp40.c - Support for Sensirion SGP40 Gas Sensor
 *
 * Copyright (C) 2021 Andreas Klinger <ak@it-klinger.de>
 *
 * I2C slave address: 0x59
 *
 * Datasheet can be found here:
 * https://www.sensirion.com/file/datasheet_sgp40
 *
 * There are two functionalities supported:
 *
 * 1) read raw logarithmic resistance value from sensor
 *    --> useful to pass it to the algorithm of the sensor vendor for
 *    measuring deteriorations and improvements of air quality.
 *    It can be read from the attribute in_resistance_raw.
 *
 * 2) calculate an estimated absolute voc index (in_concentration_input)
 *    with 0 - 500 index points) for measuring the air quality.
 *    For this purpose the value of the resistance for which the voc index
 *    will be 250 can be set up using in_resistance_calibbias (default 30000).
 *
 *    The voc index is calculated as:
 *      x = (in_resistance_raw - in_resistance_calibbias) * 0.65
 *      in_concentration_input = 500 / (1 + e^x)
 *
 * Compensation values of relative humidity and temperature can be set up
 * by writing to the out values of temp and humidityrelative.
 */

#include "linux/device.h"
#include <linux/delay.h>
#include <linux/crc8.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

/*
 * floating point calculation of voc is done as integer
 * where numbers are multiplied by 1 << SGP40_CALC_POWER
 */
#define SGP40_CALC_POWER	14

#define SGP40_CRC8_POLYNOMIAL	0x31
#define SGP40_CRC8_INIT		0xff

DECLARE_CRC8_TABLE(sgp40_crc8_table);

struct sgp40_data {
	struct device		*dev;
	struct i2c_client	*client;
	int			rht;
	int			temp;
	int			res_calibbias;
	u64			serial_number;
	/* Prevent concurrent access to rht, tmp, calibbias */
	struct mutex		lock;
};

struct sgp40_command {
	u8	command[2];
	__be16	rht_ticks;
	u8	rht_crc;
	__be16	temp_ticks;
	u8	temp_crc;
} __packed;

struct sgp40_tg_result {
	__be16	res_ticks;
	u8	res_crc;
} __packed;

/*
 * Datasheet table 16. Serial number is given as 48-bit value 0xAAAABBBBCCCC.
 */
struct sgp40_serial_number_result {
	__be16	A;
	u8	A_crc;
	__be16	B;
	u8	B_crc;
	__be16	C;
	u8	C_crc;
} __packed;

/*
 * Datasheet table 13.
 */
#define SGP40_SELF_TEST_PASS 0xD4
#define SGP40_SELF_TEST_FAIL 0x4B

struct sgp40_self_test_result {
	u8	data;
	u8	_ignore;
	u8	crc;
} __packed;


static const struct iio_chan_spec sgp40_channels[] = {
	{
		.type = IIO_CONCENTRATION,
		.channel2 = IIO_MOD_VOC,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
	{
		.type = IIO_RESISTANCE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_CALIBBIAS),
	},
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.output = 1,
	},
	{
		.type = IIO_HUMIDITYRELATIVE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.output = 1,
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

static u32 sgp40_exp(int exp, u32 power, u32 rounds)
{
        u32 x, y, xp;
        u32 factorial, divider, xmax;
        int sign = 1;
	int i;

        if (exp == 0)
                return 1 << power;
        else if (exp < 0) {
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

static int sgp40_calc_voc(struct sgp40_data *data, u16 resistance_raw, int *voc)
{
	int x;
	u32 exp = 0;

	/* we calculate as a multiple of 16384 (2^14) */
	mutex_lock(&data->lock);
	x = ((int)resistance_raw - data->res_calibbias) * 106;
	mutex_unlock(&data->lock);

	/* voc = 500 / (1 + e^x) */
	exp = sgp40_exp(x, SGP40_CALC_POWER, 18);
	*voc = 500 * ((1 << (SGP40_CALC_POWER * 2)) / ((1<<SGP40_CALC_POWER) + exp));

	dev_dbg(data->dev, "raw: %d res_calibbias: %d x: %d exp: %d voc: %d\n",
				resistance_raw, data->res_calibbias, x, exp, *voc);

	return 0;
}

static int sgp40_get_serial_number(struct sgp40_data *data)
{
	int ret;
	struct i2c_client *client = data->client;
	struct sgp40_command get_sn = {.command = {0x36, 0x82}};
	struct sgp40_serial_number_result res;

	ret = i2c_master_send(client, (char*)&get_sn, sizeof(get_sn.command));
	if (ret != sizeof(get_sn.command)) {
		dev_err(data->dev, "i2c_master_send ret: %d, expected %zu", ret, sizeof(get_sn.command));
		return -EIO;
	}
	msleep(1);
	ret = i2c_master_recv(client, (char*)&res, sizeof(res));
	if (ret < 0)
		return ret;
	if (ret != sizeof(res)) {
		dev_err(data->dev, "i2c_master_recv ret: %d, expected: %zu", ret, sizeof(res));
		return -EIO;
	}

	if (crc8(sgp40_crc8_table, (u8*)&res.A, 2, SGP40_CRC8_INIT) != res.A_crc ||
	    crc8(sgp40_crc8_table, (u8*)&res.B, 2, SGP40_CRC8_INIT) != res.B_crc ||
	    crc8(sgp40_crc8_table, (u8*)&res.C, 2, SGP40_CRC8_INIT) != res.C_crc)
	{
		dev_warn(data->dev, "CRC error in get_serial_number");
	}

	data->serial_number = 0LL | ((u64)be16_to_cpu(res.A) << 32) | ((u64)be16_to_cpu(res.B) << 16) | (u64)be16_to_cpu(res.C);
	dev_dbg(data->dev, "serial number: %llu", data->serial_number);

	return 0;
}

static int sgp40_execute_self_test(struct sgp40_data *data)
{
	int ret;
	struct i2c_client *client = data->client;
	struct sgp40_command run_test = {.command = {0x28, 0x0E}};
	struct sgp40_self_test_result res;

	ret = i2c_master_send(client, (char*)&run_test, sizeof(run_test.command));
	if (ret != sizeof(run_test.command)) {
		dev_err(data->dev, "i2c_master_send ret: %d, expected %zu", ret, sizeof(run_test.command));
		return -EIO;
	}
	msleep(320);

	ret = i2c_master_recv(client, (char*)&res, sizeof(res));
	if (ret < 0)
		return ret;
	if (ret != sizeof(res)) {
		dev_err(data->dev, "i2c_master_recv ret: %d, expected: %zu", ret, sizeof(res));
		return -EIO;
	}

	if (crc8(sgp40_crc8_table, (u8*)&res, 2, SGP40_CRC8_INIT) != res.crc) {
		dev_warn(data->dev, "CRC error in execute_self_test");
	}

	dev_dbg(data->dev, "self test result: 0x%x", res.data);
	switch (res.data) {
		case SGP40_SELF_TEST_PASS:
			return 0;
		case SGP40_SELF_TEST_FAIL:
		default:
			return res.data;
	}
}

static int sgp40_turn_heater_off(struct sgp40_data *data)
{
	int ret;
	struct i2c_client *client = data->client;
	struct sgp40_command turn_off = {.command = {0x36, 0x15}};

	ret = i2c_master_send(client, (char*)&turn_off, sizeof(turn_off.command));
	if (ret != sizeof(turn_off.command)) {
		dev_err(data->dev, "i2c_master_send ret: %d, expected %zu", ret, sizeof(turn_off.command));
		return -EIO;
	}
	msleep(1);
	return 0;
}

static int sgp40_measure_resistance_raw(struct sgp40_data *data, u16 *resistance_raw)
{
	int ret;
	struct i2c_client *client = data->client;
	u32 ticks;
	u16 ticks16;
	u8 crc;
	struct sgp40_command tg = {.command = {0x26, 0x0F}};
	struct sgp40_tg_result tgres;

	mutex_lock(&data->lock);

	ticks = (data->rht / 10) * 65535 / 10000;
	ticks16 = (u16)clamp(ticks, 0u, 65535u); /* clamp between 0 .. 100 %rH */
	tg.rht_ticks = cpu_to_be16(ticks16);
	tg.rht_crc = crc8(sgp40_crc8_table, (u8 *)&tg.rht_ticks, 2, SGP40_CRC8_INIT);

	ticks = ((data->temp + 45000) / 10 ) * 65535 / 17500;
	ticks16 = (u16)clamp(ticks, 0u, 65535u); /* clamp between -45 .. +130 °C */
	tg.temp_ticks = cpu_to_be16(ticks16);
	tg.temp_crc = crc8(sgp40_crc8_table, (u8 *)&tg.temp_ticks, 2, SGP40_CRC8_INIT);

	mutex_unlock(&data->lock);

	ret = i2c_master_send(client, (const char *)&tg, sizeof(tg));
	if (ret != sizeof(tg)) {
		dev_warn(data->dev, "i2c_master_send ret: %d sizeof: %zu\n", ret, sizeof(tg));
		return -EIO;
	}
	msleep(30);

	ret = i2c_master_recv(client, (u8 *)&tgres, sizeof(tgres));
	if (ret < 0)
		return ret;
	if (ret != sizeof(tgres)) {
		dev_warn(data->dev, "i2c_master_recv ret: %d sizeof: %zu\n", ret, sizeof(tgres));
		return -EIO;
	}

	crc = crc8(sgp40_crc8_table, (u8 *)&tgres.res_ticks, 2, SGP40_CRC8_INIT);
	if (crc != tgres.res_crc) {
		dev_err(data->dev, "CRC error while measure-raw\n");
		return -EIO;
	}

	*resistance_raw = be16_to_cpu(tgres.res_ticks);

	return 0;
}

static int sgp40_read_raw(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan, int *val,
			int *val2, long mask)
{
	struct sgp40_data *data = iio_priv(indio_dev);
	int ret, voc;
	u16 resistance_raw;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_RESISTANCE:
			ret = sgp40_measure_resistance_raw(data, &resistance_raw);
			if (ret)
				return ret;

			*val = resistance_raw;
			return IIO_VAL_INT;
		case IIO_TEMP:
			mutex_lock(&data->lock);
			*val = data->temp;
			mutex_unlock(&data->lock);
			return IIO_VAL_INT;
		case IIO_HUMIDITYRELATIVE:
			mutex_lock(&data->lock);
			*val = data->rht;
			mutex_unlock(&data->lock);
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_PROCESSED:
		ret = sgp40_measure_resistance_raw(data, &resistance_raw);
		if (ret)
			return ret;

		ret = sgp40_calc_voc(data, resistance_raw, &voc);
		if (ret)
			return ret;

		*val = voc / (1 << SGP40_CALC_POWER);
		/*
		 * calculation should fit into integer, where:
		 * voc <= (500 * 2^SGP40_CALC_POWER) = 8192000
		 * (with SGP40_CALC_POWER = 14)
		 */
		*val2 = ((voc % (1 << SGP40_CALC_POWER)) * 244) / (1 << (SGP40_CALC_POWER - 12));
		dev_dbg(data->dev, "voc: %d val: %d.%06d\n", voc, *val, *val2);
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_CALIBBIAS:
		mutex_lock(&data->lock);
		*val = data->res_calibbias;
		mutex_unlock(&data->lock);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int sgp40_write_raw(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan, int val,
			int val2, long mask)
{
	struct sgp40_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_TEMP:
			if ((val < -45000) || (val > 130000))
				return -EINVAL;

			mutex_lock(&data->lock);
			data->temp = val;
			mutex_unlock(&data->lock);
			return 0;
		case IIO_HUMIDITYRELATIVE:
			if ((val < 0) || (val > 100000))
				return -EINVAL;

			mutex_lock(&data->lock);
			data->rht = val;
			mutex_unlock(&data->lock);
			return 0;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_CALIBBIAS:
		if ((val < 20000) || (val > 52768))
			return -EINVAL;

		mutex_lock(&data->lock);
		data->res_calibbias = val;
		mutex_unlock(&data->lock);
		return 0;
	}
	return -EINVAL;
}

static ssize_t serial_number_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct sgp40_data *data = iio_priv(dev_to_iio_dev(dev));

	return sysfs_emit_at(buf, 0, "%llu\n", data->serial_number);
}

static ssize_t turn_heater_off_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct sgp40_data *data = iio_priv(dev_to_iio_dev(dev));
	sgp40_turn_heater_off(data);

	return len;
}

static IIO_DEVICE_ATTR_RO(serial_number, 0);
static IIO_DEVICE_ATTR_WO(turn_heater_off, 0);

static struct attribute *sgp40_attributes[] = {
	&iio_dev_attr_serial_number.dev_attr.attr,
	&iio_dev_attr_turn_heater_off.dev_attr.attr,
	NULL
};

static struct attribute_group sgp40_attribute_group = {
	.attrs = sgp40_attributes,
};


static const struct iio_info sgp40_info = {
	.read_raw	= sgp40_read_raw,
	.write_raw	= sgp40_write_raw,
	.attrs		= &sgp40_attribute_group,
};

static int sgp40_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct sgp40_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	data->dev = dev;

	crc8_populate_msb(sgp40_crc8_table, SGP40_CRC8_POLYNOMIAL);

	mutex_init(&data->lock);

	/* set default values */
	data->rht = 50000;		/* 50 % */
	data->temp = 25000;		/* 25 °C */
	data->res_calibbias = 30000;	/* resistance raw value for voc index of 250 */

	indio_dev->info = &sgp40_info;
	indio_dev->name = id->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = sgp40_channels;
	indio_dev->num_channels = ARRAY_SIZE(sgp40_channels);

	ret = sgp40_get_serial_number(data);
	if (ret)
		dev_warn(dev, "failed to retrieve device serial number\n");

	ret = sgp40_execute_self_test(data);
	if (ret)
		dev_warn(dev, "device self test failed: ret 0x%x", ret);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		dev_err(dev, "failed to register iio device\n");

	return ret;
}

static const struct i2c_device_id sgp40_id[] = {
	{ .name = "sgp40" },
	{ }
};

MODULE_DEVICE_TABLE(i2c, sgp40_id);

static const struct of_device_id sgp40_dt_ids[] = {
	{ .compatible = "sensirion,sgp40" },
	{ }
};

MODULE_DEVICE_TABLE(of, sgp40_dt_ids);

static struct i2c_driver sgp40_driver = {
	.driver = {
		.name = "sgp40",
		.of_match_table = sgp40_dt_ids,
	},
	.probe = sgp40_probe,
	.id_table = sgp40_id,
};
module_i2c_driver(sgp40_driver);

MODULE_AUTHOR("Andreas Klinger <ak@it-klinger.de>");
MODULE_DESCRIPTION("Sensirion SGP40 gas sensor");
MODULE_LICENSE("GPL v2");
