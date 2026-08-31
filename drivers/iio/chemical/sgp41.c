// SPDX-License-Identifier: GPL-2.0

#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define SGP41_CALC_POWER		14

#define SGP41_CRC8_POLYNOMIAL		0x31
#define SGP41_CRC8_INIT		0xff

#define SGP41_COND_ITERATIONS		10

DECLARE_CRC8_TABLE(sgp41_crc8_table);

struct sgp41_data {
	struct device *dev;
	struct i2c_client *client;

	int rht;
	int temp;

	int vos_res_calibbias;
	int nox_res_calibbias;

	struct mutex lock;/* Protects sensor state and I2C transaction.*/
	bool conditioned;
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
		.type = IIO_RESISTANCE,
		.channel = 0,
		.modified = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_RESISTANCE,
		.channel = 1,
		.indexed = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
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

static int sgp41_run_conditioning(struct sgp41_data *data)
{
	struct sgp41_tg_measure tg = {
		.command = { 0x26, 0x12 },
	};
	int i;
	int ret;

	for (i = 0; i < SGP41_COND_ITERATIONS; i++) {
		temp_rht_payload(data, &tg);

		ret = i2c_master_send(data->client, (const char *)&tg,
				      sizeof(tg));
		if (ret != sizeof(tg)) {
			dev_warn(data->dev,
				 "i2c_master_send ret: %d sizeof: %zu\n",
				 ret, sizeof(tg));
			return ret < 0 ? ret : -EIO;
		}

		msleep(50);

		if (i != SGP41_COND_ITERATIONS - 1)
			msleep(950);
	}

	data->conditioned = true;

	return 0;
}

static int sgp41_measure_raw(struct sgp41_data *data,
					u16 *voc_raw,
					u16 *nox_raw)
{
	struct sgp41_tg_measure tg = {
		.command = { 0x26, 0x19 },
	};
	struct sgp41_tg_result tgres;
	u8 crc;
	int ret;

	mutex_lock(&data->lock);

	if (!data->conditioned) {
		ret = sgp41_run_conditioning(data);
		if (ret)
			goto unlock;
	}

	temp_rht_payload(data, &tg);

	ret = i2c_master_send(data->client, (const char *)&tg, sizeof(tg));
	if (ret != sizeof(tg)) {
		dev_warn(data->dev,
			 "i2c_master_send ret: %d sizeof: %zu\n",
			 ret, sizeof(tg));
		ret = ret < 0 ? ret : -EIO;
		goto unlock;
	}

	msleep(50);

	ret = i2c_master_recv(data->client, (u8 *)&tgres, sizeof(tgres));
	if (ret < 0)
		goto unlock;

	if (ret != sizeof(tgres)) {
		dev_warn(data->dev,
			 "i2c_master_recv ret: %d sizeof: %zu\n",
			 ret, sizeof(tgres));
		ret = -EIO;
		goto unlock;
	}

	crc = crc8(sgp41_crc8_table, (u8 *)&tgres.voc_res_ticks, 2,
		   SGP41_CRC8_INIT);
	if (crc != tgres.voc_res_crc) {
		dev_err(data->dev, "CRC error while measure-voc_raw\n");
		ret = -EIO;
		goto unlock;
	}

	crc = crc8(sgp41_crc8_table, (u8 *)&tgres.nox_res_ticks, 2,
		   SGP41_CRC8_INIT);
	if (crc != tgres.nox_res_crc) {
		dev_err(data->dev, "CRC error while measure-nox_raw\n");
		ret = -EIO;
		goto unlock;
	}

	*voc_raw = be16_to_cpu(tgres.voc_res_ticks);
	*nox_raw = be16_to_cpu(tgres.nox_res_ticks);
	ret = 0;

unlock:
	mutex_unlock(&data->lock);

	return ret;
}

static int sgp41_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan, int *val,
			  int *val2, long mask)
{
	struct sgp41_data *data = iio_priv(indio_dev);
	u16 voc_raw;
	u16 nox_raw;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_RESISTANCE:
			ret = sgp41_measure_raw(data,
							   &voc_raw,
							   &nox_raw);
			if (ret)
				return ret;

			switch (chan->channel) {
			case 0:
				*val = voc_raw;
				return IIO_VAL_INT;
			case 1:
				*val = nox_raw;
				return IIO_VAL_INT;
			default:
				return -EINVAL;
			}

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
	default:
		return -EINVAL;
	}
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

	dev_dbg(data->dev, "heater turned off\n");

	return 0;
}

static void sgp41_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct sgp41_data *data = iio_priv(indio_dev);

	sgp41_heater_off(data);
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

	crc8_populate_msb(sgp41_crc8_table, SGP41_CRC8_POLYNOMIAL);

	mutex_init(&data->lock);

	data->rht = 50000;
	data->temp = 25000;

	indio_dev->info = &sgp41_info;

	if (id)
		indio_dev->name = id->name;
	else
		indio_dev->name = "sgp41";

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = sgp41_channels;
	indio_dev->num_channels = ARRAY_SIZE(sgp41_channels);

	ret = devm_iio_device_register(dev, indio_dev);
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

static struct i2c_driver sgp41_driver = {
	.driver = {
		.name = "sgp41",
		.of_match_table = sgp41_dt_ids,
	},
	.probe = sgp41_probe,
	.id_table = sgp41_id,
	.remove = sgp41_remove,
};

module_i2c_driver(sgp41_driver);

MODULE_AUTHOR("Akshat Chandra <notmissinge@gmail.com>");
MODULE_DESCRIPTION("Sensirion SGP41 gas sensor");
MODULE_LICENSE("GPL v2");
