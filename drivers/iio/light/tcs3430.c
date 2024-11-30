// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Paul Kocialkowski <paulk@sys-base.io>
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#define TCS3430_ENABLE			0x80
#define TCS3430_ENABLE_PON		BIT(0)
#define TCS3430_ENABLE_AEN		BIT(1)
#define TCS3430_ENABLE_WEN		BIT(3)

#define TCS3430_ATIME			0x81
#define TCS3430_ATIME_CYCLES(v)		(((v) - 1) & GENMASK(7, 0))

#define TCS3430_WTIME			0x83
#define TCS3430_WTIME_CYCLES(v)		(((v) - 1) & GENMASK(7, 0))

#define TCS3430_AILTL			0x84
#define TCS3430_AILTH			0x85
#define TCS3430_AIHTL			0x86
#define TCS3430_AIHTH			0x87

#define TCS3430_PERS			0x8c
#define TCS3430_PERS_EVERY_CYCLE	0
#define TCS3430_PERS_OUTSIDE_THRESHOLD	1

#define TCS3430_CFG0			0x8d
#define TCS3430_CFG0_WLONG		BIT(2)
#define TCS3430_CFG0_RESERVED		(0x10 << 3)

#define TCS3430_CFG1			0x90
#define TCS3430_CFG1_AGAIN_1X		0
#define TCS3430_CFG1_AGAIN_4X		1
#define TCS3430_CFG1_AGAIN_16X		2
#define TCS3430_CFG1_AGAIN_64X		3
#define TCS3430_CFG1_AMUX_X		0
#define TCS3430_CFG1_AMUX_IR2		BIT(3)

#define TCS3430_REVID			0x91

#define TCS3430_ID			0x92
#define TCS3430_ID_VALUE(v)		(((v) & GENMASK(7, 2)) >> 2)
#define TCS3430_ID_TCS3430		0x37

#define TCS3430_STATUS			0x93
#define TCS3430_STATUS_AINT		BIT(4)
#define TCS3430_STATUS_ASAT		BIT(7)

#define TCS3430_CH0DATAL		0x94
#define TCS3430_CH0DATAH		0x95
#define TCS3430_CH1DATAL		0x96
#define TCS3430_CH1DATAH		0x97
#define TCS3430_CH2DATAL		0x98
#define TCS3430_CH2DATAH		0x99
#define TCS3430_CH3DATAL		0x9a
#define TCS3430_CH3DATAH		0x9b

#define TCS3430_CFG2			0x9f
#define TCS3430_CFG2_RESERVED		0x4
#define TCS3430_CFG2_HGAIN_128X		BIT(4)

#define TCS3430_CFG3			0xab
#define TCS3430_CFG3_RESERVED		0xc
#define TCS3430_CFG3_SAI		BIT(4)
#define TCS3430_CFG3_INT_READ_CLEAR	BIT(7)

#define TCS3430_AZ_CONFIG		0xd6
#define TCS3430_AZ_CONFIG_NTH(v)	((v) & GENMASK(6, 0))
#define TCS3430_AZ_CONFIG_NTH_FIRST	0x7f
#define TCS3430_AZ_CONFIG_START_ZERO	0
#define TCS3430_AZ_CONFIG_START_OFFSET	BIT(7)

#define TCS3430_INTENAB			0xdd
#define TCS3430_INTENAB_AIEN		BIT(4)
#define TCS3430_INTENAB_ASIEN		BIT(7)

#define TCS3430_ATIME_CYCLE_TIME_US	2780
#define TCS3430_ATIME_CYCLES_MAX	256

struct tcs3430_sensor_state {
	bool active;
	/* integration time */
	u16 atime_cycles;
	/* wait time */
	u16 wtime_cycles;
	bool wtime_long;
	bool wtime_enabled;
	/* analog gain */
	u8 again;
};

struct tcs3430_sensor_scan {
	u16 channels[4];
	s64 timestamp __aligned(8);
};

struct tcs3430_sensor {
	struct device *dev;
	struct i2c_client *i2c_client;
	struct iio_dev *iio_dev;

	struct regulator *vdd_supply;

	struct tcs3430_sensor_state state;
	struct tcs3430_sensor_scan scan;
	int atime_available[2 * TCS3430_ATIME_CYCLES_MAX];
	bool scan_sync;
	struct mutex lock;
};

static int tcs3430_state_configure_power(struct tcs3430_sensor *sensor);

static const int tcs3430_again_values[] = {
	1, 4, 16, 64, 128
};

static void tcs3430_atime_available_setup(struct tcs3430_sensor *sensor)
{
	unsigned int atime_us;
	unsigned int index;
	unsigned int i;

	for (i = 0; i < TCS3430_ATIME_CYCLES_MAX; i++) {
		atime_us = (i + 1) * TCS3430_ATIME_CYCLE_TIME_US;

		sensor->atime_available[index] = atime_us / USEC_PER_SEC;
		index++;
		sensor->atime_available[index] = atime_us % USEC_PER_SEC;
		index++;
	}
}

static int tcs3430_data_scan(struct tcs3430_sensor *sensor,
			     struct tcs3430_sensor_scan *scan)
{
	struct i2c_client *i2c_client = sensor->i2c_client;
	int channels_map[] = { 2, 1, 3, 0 };
	u8 values[8] = { 0 };
	unsigned int index = 0;
	unsigned int i;
	int ret;

	/* Read all values in one transaction to ensure coherency. */
	ret = i2c_smbus_read_i2c_block_data(i2c_client, TCS3430_CH0DATAL, 8,
					    values);
	if (ret < 0)
		return ret;

	for (i = 0; i < 4; i++) {
		int scan_index;
		u16 value;

		value = values[index + 1] << 8 | values[index];
		index += 2;

		scan_index = channels_map[i];
		if (scan_index < 0)
			continue;

		scan->channels[scan_index] = value;
	}

	return 0;
}

static int tcs3430_identify(struct tcs3430_sensor *sensor)
{
	struct device *dev = sensor->dev;
	int ret;
	u8 id;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_read_byte_data(sensor->i2c_client, TCS3430_ID);
	if (ret < 0)
		goto complete;

	id = (u8)TCS3430_ID_VALUE(ret);

	switch (id) {
	case TCS3430_ID_TCS3430:
		dev_info(dev, "identified TCS3430 sensor\n");
		ret = 0;
		break;
	default:
		dev_err(dev, "unknown sensor with id: %#x\n", id);
		ret = -ENODEV;
		break;
	}

complete:
	pm_runtime_put_sync(dev);

	return ret;
}

static int tcs3430_power(struct tcs3430_sensor *sensor, bool on)
{
	struct tcs3430_sensor_state *state = &sensor->state;
	int ret;

	mutex_lock(&sensor->lock);

	state->active = on;
	ret = tcs3430_state_configure_power(sensor);

	mutex_unlock(&sensor->lock);

	return ret;
}

/* State */

static int tcs3430_state_configure_power(struct tcs3430_sensor *sensor)
{
	struct tcs3430_sensor_state *state = &sensor->state;
	struct i2c_client *i2c_client = sensor->i2c_client;
	u8 value;
	int ret;

	ret = i2c_smbus_read_byte_data(i2c_client, TCS3430_ENABLE);
	if (ret < 0)
		return ret;

	value = (u8)ret;

	if (state->active)
		value |= TCS3430_ENABLE_PON | TCS3430_ENABLE_AEN;
	else
		value &= ~(TCS3430_ENABLE_PON | TCS3430_ENABLE_AEN);

	ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_ENABLE, value);
	if (ret < 0)
		return ret;

	return 0;
}

static int tcs3430_state_configure_atime(struct tcs3430_sensor *sensor)
{
	struct tcs3430_sensor_state *state = &sensor->state;
	struct i2c_client *i2c_client = sensor->i2c_client;
	u8 value;
	int ret;

	value = TCS3430_ATIME_CYCLES(state->atime_cycles);

	ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_ATIME, value);
	if (ret < 0)
		return ret;

	return 0;
}

static int tcs3430_state_configure_wtime(struct tcs3430_sensor *sensor)
{
	struct tcs3430_sensor_state *state = &sensor->state;
	struct i2c_client *i2c_client = sensor->i2c_client;
	u8 value;
	int ret;

	if (state->wtime_long) {
		value = TCS3430_CFG0_WLONG | TCS3430_CFG0_RESERVED;

		ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_CFG0,
						value);
		if (ret < 0)
			return ret;
	}

	value = TCS3430_WTIME_CYCLES(state->wtime_cycles);

	ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_WTIME, value);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_read_byte_data(sensor->i2c_client, TCS3430_ENABLE);
	if (ret < 0)
		return ret;

	value = (u8)ret;

	if (state->wtime_enabled)
		value |= TCS3430_ENABLE_WEN;
	else
		value &= ~TCS3430_ENABLE_WEN;

	ret = i2c_smbus_write_byte_data(sensor->i2c_client, TCS3430_ENABLE,
					value);
	if (ret < 0)
		return ret;

	return 0;
}

static int tcs3430_state_configure_again(struct tcs3430_sensor *sensor)
{
	struct tcs3430_sensor_state *state = &sensor->state;
	struct i2c_client *i2c_client = sensor->i2c_client;
	u8 value;
	int ret;

	value = TCS3430_CFG2_RESERVED;

	if (state->again == 128)
		value |= TCS3430_CFG2_HGAIN_128X;

	ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_CFG2, value);
	if (ret < 0)
		return ret;

	/* Alwawys mux CH3 to X channel. */
	value = TCS3430_CFG1_AMUX_X;

	switch (state->again) {
	case 1:
		value |= TCS3430_CFG1_AGAIN_1X;
		break;
	case 4:
		value |= TCS3430_CFG1_AGAIN_4X;
		break;
	case 16:
		value |= TCS3430_CFG1_AGAIN_16X;
		break;
	case 64:
	case 128:
		value |= TCS3430_CFG1_AGAIN_64X;
		break;
	default:
		return -EINVAL;
	}

	ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_CFG1, value);
	if (ret < 0)
		return ret;

	return 0;
}

static int tcs3430_state_configure_az(struct tcs3430_sensor *sensor)
{
	struct i2c_client *i2c_client = sensor->i2c_client;
	u8 value;
	int ret;

	/* Only run auto-zero at first cycle, start search at zero. */
	value = TCS3430_AZ_CONFIG_NTH_FIRST | TCS3430_AZ_CONFIG_START_ZERO;

	ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_AZ_CONFIG, value);
	if (ret < 0)
		return ret;

	return 0;
}

static int tcs3430_state_configure_interrupt(struct tcs3430_sensor *sensor)
{
	struct i2c_client *i2c_client = sensor->i2c_client;
	u8 value;
	int ret;

	if (i2c_client->irq)
		value = TCS3430_INTENAB_AIEN;
	else
		value = 0;

	ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_INTENAB, value);
	if (ret < 0)
		return ret;

	/* Clear flags with status read, don't sleep after interrupt. */
	value = TCS3430_CFG3_RESERVED | TCS3430_CFG3_INT_READ_CLEAR;

	ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_CFG3, value);
	if (ret < 0)
		return ret;

	/* Enable interrupt for every ALS cycle. */
	value = TCS3430_PERS_EVERY_CYCLE;
	ret = i2c_smbus_write_byte_data(i2c_client, TCS3430_PERS, value);
	if (ret < 0)
		return ret;

	return 0;
}

static int tcs3430_state_configure(struct tcs3430_sensor *sensor)
{
	int ret;

	mutex_lock(&sensor->lock);

	/* Configure integration time. */
	ret = tcs3430_state_configure_atime(sensor);
	if (ret)
		goto complete;

	/* Configure wait time. */
	ret = tcs3430_state_configure_wtime(sensor);
	if (ret)
		goto complete;

	/* Configure analog gain. */
	ret = tcs3430_state_configure_again(sensor);
	if (ret)
		goto complete;

	/* Configure auto-zero. */
	ret = tcs3430_state_configure_az(sensor);
	if (ret)
		goto complete;

	/* Configure interrupt. */
	ret = tcs3430_state_configure_interrupt(sensor);
	if (ret)
		goto complete;

	/* Configure power. */
	ret = tcs3430_state_configure_power(sensor);
	if (ret)
		goto complete;

complete:
	mutex_unlock(&sensor->lock);

	return ret;
}

static void tcs3430_state_reset(struct tcs3430_sensor *sensor)
{
	struct tcs3430_sensor_state *state = &sensor->state;

	/* At least 64 cycles are required to reach full scale. */
	state->atime_cycles = 64;

	/* No wait time between samples outside of trigger mode. */
	state->wtime_cycles = 0;
	state->wtime_long = false;
	state->wtime_enabled = false;

	/* Start with a low gain. */
	state->again = 4;
}

/* IIO */

static const struct iio_chan_spec tcs3430_iio_channels[] = {
	{
		.type			= IIO_INTENSITY,
		.channel2		= IIO_MOD_X,
		.address		= TCS3430_CH3DATAL,
		.modified		= 1,

		.scan_index		= 0,
		.scan_type		= {
			.sign		= 'u',
			.realbits	= 16,
			.storagebits	= 16,
			.endianness	= IIO_LE,
		},

		.info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type =
			BIT(IIO_CHAN_INFO_INT_TIME) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
		.info_mask_shared_by_type_available =
			BIT(IIO_CHAN_INFO_INT_TIME) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
	},
	{
		.type			= IIO_INTENSITY,
		.channel2		= IIO_MOD_Y,
		.address		= TCS3430_CH1DATAL,
		.modified		= 1,

		.scan_index		= 1,
		.scan_type		= {
			.sign		= 'u',
			.realbits	= 16,
			.storagebits	= 16,
			.endianness	= IIO_LE,
		},

		.info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type =
			BIT(IIO_CHAN_INFO_INT_TIME) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
		.info_mask_shared_by_type_available =
			BIT(IIO_CHAN_INFO_INT_TIME) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
	},
	{
		.type			= IIO_INTENSITY,
		.channel2		= IIO_MOD_Z,
		.address		= TCS3430_CH0DATAL,
		.modified		= 1,

		.scan_index		= 2,
		.scan_type		= {
			.sign		= 'u',
			.realbits	= 16,
			.storagebits	= 16,
			.endianness	= IIO_LE,
		},

		.info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type =
			BIT(IIO_CHAN_INFO_INT_TIME) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
		.info_mask_shared_by_type_available =
			BIT(IIO_CHAN_INFO_INT_TIME) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
	},
	{
		.type			= IIO_INTENSITY,
		.channel2		= IIO_MOD_LIGHT_IR,
		.address		= TCS3430_CH2DATAL,
		.modified		= 1,

		.scan_index		= 3,
		.scan_type		= {
			.sign		= 'u',
			.realbits	= 16,
			.storagebits	= 16,
			.endianness	= IIO_LE,
		},

		.info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type =
			BIT(IIO_CHAN_INFO_INT_TIME) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
		.info_mask_shared_by_type_available =
			BIT(IIO_CHAN_INFO_INT_TIME) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
	},
	IIO_CHAN_SOFT_TIMESTAMP(4),
};

static int tcs3430_iio_read_raw(struct iio_dev *iio_dev,
				struct iio_chan_spec const *channel,
				int *value_first, int *value_second, long mask)
{
	struct tcs3430_sensor *sensor = iio_priv(iio_dev);
	struct tcs3430_sensor_state *state = &sensor->state;
	struct i2c_client *i2c_client = sensor->i2c_client;
	struct device *dev = sensor->dev;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		unsigned int scan_index;

		ret = iio_device_claim_direct_mode(iio_dev);
		if (ret)
			return ret;

		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			goto release_direct_mode;

		if (sensor->scan_sync) {
			scan_index = channel->scan_index;

			mutex_lock(&sensor->lock);
			ret = (int)sensor->scan.channels[scan_index];
			mutex_unlock(&sensor->lock);
		} else {
			ret = i2c_smbus_read_word_data(i2c_client,
						       channel->address);
		}

		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);

release_direct_mode:
		iio_device_release_direct_mode(iio_dev);

		if (ret < 0)
			return ret;

		*value_first = ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_INT_TIME:
		unsigned int atime_us = state->atime_cycles *
					TCS3430_ATIME_CYCLE_TIME_US;

		*value_first = atime_us / USEC_PER_SEC;
		*value_second = atime_us % USEC_PER_SEC;

		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_HARDWAREGAIN:
		*value_first = state->again;

		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int tcs3430_iio_read_avail(struct iio_dev *iio_dev,
				  struct iio_chan_spec const *channel,
				  const int **values, int *type, int *length,
				  long mask)
{
	struct tcs3430_sensor *sensor = iio_priv(iio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		*length = ARRAY_SIZE(tcs3430_again_values);
		*values = tcs3430_again_values;
		*type = IIO_VAL_INT;
		return IIO_AVAIL_LIST;

	case IIO_CHAN_INFO_INT_TIME:
		*length = ARRAY_SIZE(sensor->atime_available);
		*values = sensor->atime_available;
		*type = IIO_VAL_INT_PLUS_MICRO;
		return IIO_AVAIL_LIST;

	default:
		return -EINVAL;
	}
}

static int tcs3430_iio_write_raw(struct iio_dev *iio_dev,
				 struct iio_chan_spec const *channel,
				 int value_first, int value_second, long mask)
{
	struct tcs3430_sensor *sensor = iio_priv(iio_dev);
	struct tcs3430_sensor_state *state = &sensor->state;
	struct device *dev = sensor->dev;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		unsigned int atime_us = value_first * USEC_PER_SEC +
					value_second;
		unsigned int cycles = atime_us / TCS3430_ATIME_CYCLE_TIME_US;
		unsigned int rest = atime_us % TCS3430_ATIME_CYCLE_TIME_US;

		if (rest || !cycles || cycles > TCS3430_ATIME_CYCLES_MAX)
			return -EINVAL;

		state->atime_cycles = (u16)cycles;

		if (pm_runtime_suspended(dev))
			return 0;

		ret = tcs3430_state_configure_atime(sensor);
		if (ret)
			return ret;

		return 0;

	case IIO_CHAN_INFO_HARDWAREGAIN:
		unsigned int i;
		bool valid = false;

		for (i = 0; i < ARRAY_SIZE(tcs3430_again_values); i++) {
			if (tcs3430_again_values[i] == value_first) {
				valid = true;
				break;
			}
		}

		if (!valid)
			return -EINVAL;

		state->again = (u16)value_first;

		if (pm_runtime_suspended(dev))
			return 0;

		ret = tcs3430_state_configure_again(sensor);
		if (ret)
			return ret;

		return 0;

	default:
		return -EINVAL;
	}
}

static const struct iio_info tcs3430_iio_info = {
	.read_raw	= tcs3430_iio_read_raw,
	.read_avail	= tcs3430_iio_read_avail,
	.write_raw	= tcs3430_iio_write_raw,
};

static irqreturn_t tcs3430_iio_buffer_trigger(int irq, void *data)
{
	struct iio_poll_func *poll_func = data;
	struct iio_dev *iio_dev = poll_func->indio_dev;
	struct tcs3430_sensor *sensor = iio_priv(iio_dev);
	struct tcs3430_sensor_scan scan = { 0 };
	s64 timestamp;
	unsigned int index = 0;
	unsigned int i;
	int ret;

	/* Capture timestamp just before reading values. */
	timestamp = iio_get_time_ns(iio_dev);

	mutex_lock(&sensor->lock);

	if (!sensor->scan_sync) {
		ret = tcs3430_data_scan(sensor, &sensor->scan);
		if (ret)
			goto complete;
	}

	for_each_set_bit(i, iio_dev->active_scan_mask, iio_dev->masklength) {
		/* Assume scan index matches array index. */
		const struct iio_chan_spec *channel = &tcs3430_iio_channels[i];
		unsigned int scan_index = channel->scan_index;

		/* Only active channels are reported, in order. */
		scan.channels[index] = sensor->scan.channels[scan_index];
		index++;
	}

	iio_push_to_buffers_with_timestamp(iio_dev, &scan, timestamp);

complete:
	mutex_unlock(&sensor->lock);

	iio_trigger_notify_done(iio_dev->trig);

	return IRQ_HANDLED;
}

static int tcs3430_iio_buffer_preenable(struct iio_dev *iio_dev)
{
	struct tcs3430_sensor *sensor = iio_priv(iio_dev);
	struct device *dev = sensor->dev;

	return pm_runtime_resume_and_get(dev);
}

static int tcs3430_iio_buffer_postdisable(struct iio_dev *iio_dev)
{
	struct tcs3430_sensor *sensor = iio_priv(iio_dev);
	struct device *dev = sensor->dev;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;
}

static const struct iio_buffer_setup_ops tcs3430_iio_buffer_setup_ops = {
	.preenable = tcs3430_iio_buffer_preenable,
	.postdisable = tcs3430_iio_buffer_postdisable,
};

static int tcs3430_iio_setup(struct tcs3430_sensor *sensor)
{
	struct iio_dev *iio_dev = sensor->iio_dev;
	struct device *dev = sensor->dev;
	int ret;

	iio_dev->info = &tcs3430_iio_info;
	iio_dev->name = "tcs3430";
	iio_dev->channels = tcs3430_iio_channels;
	iio_dev->num_channels = ARRAY_SIZE(tcs3430_iio_channels);
	iio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_iio_triggered_buffer_setup(dev, iio_dev, NULL,
					      tcs3430_iio_buffer_trigger,
					      &tcs3430_iio_buffer_setup_ops);
	if (ret) {
		dev_err(dev, "failed to setup iio triggered buffer\n");
		return ret;
	}

	ret = iio_device_register(iio_dev);
	if (ret) {
		dev_err(dev, "failed to register iio device\n");
		return ret;
	}

	return 0;
}

static void tcs3430_iio_cleanup(struct tcs3430_sensor *sensor)
{
	iio_device_unregister(sensor->iio_dev);
}

/* IRQ */

static irqreturn_t tcs3430_irq(int irq, void *data)
{
	struct tcs3430_sensor *sensor = data;
	struct tcs3430_sensor_state *state = &sensor->state;
	u8 status;
	int ret;

	/* Status register is reset by reading. */
	ret = i2c_smbus_read_byte_data(sensor->i2c_client, TCS3430_STATUS);
	if (ret < 0)
		return IRQ_HANDLED;

	status = (u8)ret;

	mutex_lock(&sensor->lock);

	if (!state->active)
		goto complete;

	if (status & TCS3430_STATUS_AINT)
		tcs3430_data_scan(sensor, &sensor->scan);

complete:
	mutex_unlock(&sensor->lock);

	return IRQ_HANDLED;
}

/* PM */

static int tcs3430_suspend(struct device *dev)
{
	struct tcs3430_sensor *sensor = dev_get_drvdata(dev);
	int ret;

	ret = tcs3430_power(sensor, 0);
	if (ret)
		goto error;

	ret = regulator_disable(sensor->vdd_supply);
	if (ret)
		goto error;

	return 0;

error:
	return -EAGAIN;
}

static int tcs3430_resume(struct device *dev)
{
	struct tcs3430_sensor *sensor = dev_get_drvdata(dev);
	int ret;

	ret = regulator_enable(sensor->vdd_supply);
	if (ret)
		goto error;

	/* Wait for the regulator to settle and the chip to power-on. */
	udelay(30);

	ret = tcs3430_state_configure(sensor);
	if (ret)
		goto error_regulator;

	ret = tcs3430_power(sensor, 1);
	if (ret)
		goto error_regulator;

	/*
	 * Wait a bit longer than integration time (cycles of 2.78 ms) for the
	 * first samples to be available.
	 */
	msleep(sensor->state.atime_cycles * 3);

	return 0;

error_regulator:
	regulator_disable(sensor->vdd_supply);

error:
	return -EAGAIN;
}

static const struct dev_pm_ops tcs3430_pm_ops = {
	.runtime_suspend	= tcs3430_suspend,
	.runtime_resume		= tcs3430_resume,
};

/* I2C */

static int tcs3430_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tcs3430_sensor *sensor;
	struct iio_dev *iio_dev;
	int irq = client->irq;
	int ret;

	iio_dev = devm_iio_device_alloc(dev, sizeof(*sensor));
	if (!iio_dev)
		return -ENOMEM;

	sensor = iio_priv(iio_dev);

	sensor->dev = dev;
	sensor->i2c_client = client;
	sensor->iio_dev = iio_dev;

	i2c_set_clientdata(client, sensor);

	mutex_init(&sensor->lock);

	sensor->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(sensor->vdd_supply)) {
		dev_err(dev, "failed to get VDD regulator\n");
		return PTR_ERR(sensor->vdd_supply);
	}

	tcs3430_atime_available_setup(sensor);
	tcs3430_state_reset(sensor);

	devm_pm_runtime_enable(dev);

	ret = tcs3430_identify(sensor);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(&client->dev, 5000);
	pm_runtime_use_autosuspend(&client->dev);

	if (irq > 0) {
		ret = devm_request_threaded_irq(dev, irq, NULL, tcs3430_irq,
						IRQF_TRIGGER_LOW | IRQF_ONESHOT,
						"tcs3430", sensor);
		if (ret) {
			dev_err(dev, "failed to request irq\n");
			return ret;
		}

		sensor->scan_sync = true;
	}

	ret = tcs3430_iio_setup(sensor);
	if (ret)
		return ret;

	return 0;
}

static void tcs3430_remove(struct i2c_client *client)
{
	struct tcs3430_sensor *sensor = i2c_get_clientdata(client);

	tcs3430_iio_cleanup(sensor);
}

static const struct i2c_device_id tcs3430_id[] = {
	{ "tcs3430" },
	{ }
};

MODULE_DEVICE_TABLE(i2c, tcs3430_id);

static const struct of_device_id tcs3430_of_match[] = {
	{ .compatible = "ams,tcs3430" },
	{ }
};

MODULE_DEVICE_TABLE(of, tcs3430_of_match);

static struct i2c_driver tcs3430_driver = {
	.driver = {
		.name		= "tcs3430",
		.of_match_table = tcs3430_of_match,
		.pm		= &tcs3430_pm_ops,
	},
	.id_table	= tcs3430_id,
	.probe		= tcs3430_probe,
	.remove		= tcs3430_remove,
};

module_i2c_driver(tcs3430_driver);

MODULE_AUTHOR("Paul Kocialkowski <paulk@sys-base.io>");
MODULE_DESCRIPTION("AMS TCS3430 XYZ tristimulus color sensor driver");
MODULE_LICENSE("GPL");
