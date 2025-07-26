// SPDX-License-Identifier: GPL-2.0-only
/**
 * Driver for the Infineon TLV493D Low-Power 3D Magnetic Sensor
 *
 * Copyright (C) 2025 Dixit Parmar <dixitparmar19@gmail.com>
 *
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/units.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define TLV493D_RD_REG_BX	0x00
#define TLV493D_RD_REG_BY	0x01
#define TLV493D_RD_REG_BZ	0x02
#define TLV493D_RD_REG_TEMP	0x03
#define TLV493D_RD_REG_BX2	0x04
#define TLV493D_RD_REG_BZ2	0x05
#define TLV493D_RD_REG_TEMP2	0x06
#define TLV493D_RD_REG_RES1	0x07
#define TLV493D_RD_REG_RES2	0x08
#define TLV493D_RD_REG_RES3	0x09
#define TLV493D_RD_REG_MAX	0x0a
#define TLV493D_WR_REG_RES	0x00
#define TLV493D_WR_REG_MODE1	0x01
#define TLV493D_WR_REG_RES2	0x02
#define TLV493D_WR_REG_MODE2	0x03
#define TLV493D_WR_REG_MAX	0x04
#define TLV493D_VAL_MAG_X_AXIS_MSB	GENMASK(7, 0)
#define TLV493D_VAL_MAG_X_AXIS_LSB	GENMASK(7, 4)
#define TLV493D_VAL_MAG_Y_AXIS_MSB	GENMASK(7, 0)
#define TLV493D_VAL_MAG_Y_AXIS_LSB	GENMASK(3, 0)
#define TLV493D_VAL_MAG_Z_AXIS_MSB	GENMASK(7, 0)
#define TLV493D_VAL_MAG_Z_AXIS_LSB	GENMASK(3, 0)
#define TLV493D_VAL_TEMP_MSB		GENMASK(7, 4)
#define TLV493D_VAL_TEMP_LSB		GENMASK(7, 0)
#define TLV493D_VAL_FRAME_COUNTER	GENMASK(3, 2)
#define TLV493D_VAL_CHANNEL	GENMASK(1, 0)
#define TLV493D_VAL_PD_FLAG	BIT(4)
#define TLV493D_RD_REG_RES1_WR_MASK	GENMASK(4, 3)
#define TLV493D_RD_REG_RES2_WR_MASK	GENMASK(7, 0)
#define TLV493D_RD_REG_RES3_WR_MASK	GENMASK(4, 0)
#define TLV493D_MODE1_MOD_FAST	BIT(1)
#define TLV493D_MODE1_MOD_LOW	BIT(0)
#define TLV493D_MODE2_TEMP_CTRL	BIT(7)
#define TLV493D_MODE2_LP_PERIOD	BIT(6)
#define TLV493D_MODE2_PARITY_CTRL	BIT(5)

#define SET_BIT(b, bit)	(b |= bit)
#define CLR_BIT(b, bit)	(b &= ~bit)

#define TLV493D_DATA_X_GET(b)	\
	sign_extend32(FIELD_GET(TLV493D_VAL_MAG_X_AXIS_MSB, b[TLV493D_RD_REG_BX]) << 4 | \
			(FIELD_GET(TLV493D_VAL_MAG_X_AXIS_LSB, b[TLV493D_RD_REG_BX2]) >> 4), 11)
#define TLV493D_DATA_Y_GET(b)	\
	sign_extend32(FIELD_GET(TLV493D_VAL_MAG_Y_AXIS_MSB, b[TLV493D_RD_REG_BY]) << 4 | \
			FIELD_GET(TLV493D_VAL_MAG_Y_AXIS_LSB, b[TLV493D_RD_REG_BX2]), 11)
#define TLV493D_DATA_Z_GET(b)	\
	sign_extend32(FIELD_GET(TLV493D_VAL_MAG_Z_AXIS_MSB, b[TLV493D_RD_REG_BZ]) << 4 | \
			FIELD_GET(TLV493D_VAL_MAG_Z_AXIS_LSB, b[TLV493D_RD_REG_BZ2]), 11)
#define TLV493D_DATA_TEMP_GET(b)	\
	sign_extend32(FIELD_GET(TLV493D_VAL_TEMP_MSB, b[TLV493D_RD_REG_TEMP]) << 8 | \
			FIELD_GET(TLV493D_VAL_TEMP_LSB, b[TLV493D_RD_REG_TEMP2]), 11)

enum tlv493d_channels {
	AXIS_X = 0,
	AXIS_Y,
	AXIS_Z,
	TEMPERATURE,
};

enum tlv493d_op_mode {
	TLV493D_OP_MODE_POWERDOWN = 0,
	TLV493D_OP_MODE_FAST,
	TLV493D_OP_MODE_LOWPOWER,
	TLV493D_OP_MODE_ULTRA_LOWPOWER,
	TLV493D_OP_MODE_MASTERCONTROLLED,
	TLV493D_OP_MODE_MAX,
};

struct tlv493d_mode {
	u8 m;
	u32 sleep_us;
};

struct tlv493d_data {
	struct device *dev;
	struct i2c_client *client;
	struct mutex lock;
	struct regmap *map;
	u8 mode;
	u8 wr_regs[TLV493D_WR_REG_MAX];
	s32 temp_offset;
};

/*
 * Different mode has different measurement cycle time, this time is
 * used in deriving the sleep and timemout while reading the data from
 * sensor in polling.
 * Power-down mode: No measurement.
 * Fast mode: Freq:3.3 KHz. Measurement time:305 usec.
 * Low-power mode: Freq:100 Hz. Measurement time:10 msec.
 * Ultra low-power mode: Freq:10 Hz. Measurement time:100 msec.
 * Master controlled mode: Freq:3.3 Khz. Measurement time:305 usec.
 */
static struct tlv493d_mode modes[TLV493D_OP_MODE_MAX] = {
	{.m = TLV493D_OP_MODE_POWERDOWN, .sleep_us = 0 },
	{.m = TLV493D_OP_MODE_FAST, .sleep_us = 305 },
	{.m = TLV493D_OP_MODE_LOWPOWER, .sleep_us = 10 * USEC_PER_MSEC },
	{.m = TLV493D_OP_MODE_ULTRA_LOWPOWER, .sleep_us = 100 * USEC_PER_MSEC },
	{.m = TLV493D_OP_MODE_MASTERCONTROLLED, .sleep_us = 305 },
};

/*
 * The datasheet mentions the sensor supports only direct byte-stream write starting from
 * register address 0x0. So for any modification to be made to any write registers, it must
 * be written starting from the register address 0x0.
 * I2C write operation should not contain register address in the I2C frame, it should
 * contains only raw byte stream for the write registers. As below,
 * I2C Frame: |S|SlaveAddr Wr|Ack|Byte[0]|Ack|Byte[1]|Ack|.....|Sp|
 */
static int tlv493d_write_all_regs(struct tlv493d_data *data)
{
	int ret;

	if (!data || !data->client)
		return -EINVAL;

	/*
	 * As regmap does not provide raw write API which perform I2C write without
	 * specifying register address, direct i2c_master_send() API is used.
	 */
	ret = i2c_master_send(data->client, data->wr_regs, ARRAY_SIZE(data->wr_regs));
	if (ret < 0) {
		dev_err(data->dev, "failed to write registers. error %d\n", ret);
		return ret;
	}

	return 0;
}

static int tlv493d_set_operating_mode(struct tlv493d_data *data, u8 mode)
{
	if (!data)
		return -EINVAL;

	u8 *reg_mode1 = &data->wr_regs[TLV493D_WR_REG_MODE1];
	u8 *reg_mode2 = &data->wr_regs[TLV493D_WR_REG_MODE2];

	switch (mode) {
	case TLV493D_OP_MODE_POWERDOWN:
		CLR_BIT(*reg_mode1, TLV493D_MODE1_MOD_FAST);
		CLR_BIT(*reg_mode1, TLV493D_MODE1_MOD_LOW);
		break;

	case TLV493D_OP_MODE_FAST:
		SET_BIT(*reg_mode1, TLV493D_MODE1_MOD_FAST);
		CLR_BIT(*reg_mode1, TLV493D_MODE1_MOD_LOW);
		break;

	case TLV493D_OP_MODE_LOWPOWER:
		CLR_BIT(*reg_mode1, TLV493D_MODE1_MOD_FAST);
		SET_BIT(*reg_mode1, TLV493D_MODE1_MOD_LOW);
		SET_BIT(*reg_mode2, TLV493D_MODE2_LP_PERIOD);
		break;

	case TLV493D_OP_MODE_ULTRA_LOWPOWER:
		CLR_BIT(*reg_mode1, TLV493D_MODE1_MOD_FAST);
		SET_BIT(*reg_mode1, TLV493D_MODE1_MOD_LOW);
		CLR_BIT(*reg_mode2, TLV493D_MODE2_LP_PERIOD);
		break;

	case TLV493D_OP_MODE_MASTERCONTROLLED:
		SET_BIT(*reg_mode1, TLV493D_MODE1_MOD_FAST);
		SET_BIT(*reg_mode1, TLV493D_MODE1_MOD_LOW);
		break;

	default:
		dev_err(data->dev, "invalid mode configuration\n");
		return -EINVAL;
	}

	return tlv493d_write_all_regs(data);
}

static int tlv493d_get_measurements(struct tlv493d_data *data, s16 *x, s16 *y,
				s16 *z, s16 *t)
{
	u8 buff[7] = {0};
	int err, ret;
	struct tlv493d_mode *mode;

	if (!data)
		return -EINVAL;

	guard(mutex)(&data->lock);

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return ret;

	mode = &modes[data->mode];

	/*
	 * Poll until data is valid,
	 * For a valid data TLV493D_VAL_CHANNEL bit of TLV493D_RD_REG_TEMP should be set to 0.
	 * The sampling time depends on the sensor mode. poll 3x the time of the sampling time.
	 */
	ret = read_poll_timeout(regmap_bulk_read, err, err ||
			FIELD_GET(TLV493D_VAL_CHANNEL, buff[TLV493D_RD_REG_TEMP]) == 0,
			mode->sleep_us, (3 * mode->sleep_us), false, data->map, TLV493D_RD_REG_BX,
			buff, ARRAY_SIZE(buff));
	if (ret) {
		dev_err(data->dev, "read poll timeout, error:%d", ret);
		goto out;
	}
	if (err) {
		dev_err(data->dev, "read data failed, error:%d\n", ret);
		ret = err;
		goto out;
	}

	*x = TLV493D_DATA_X_GET(buff);
	*y = TLV493D_DATA_Y_GET(buff);
	*z = TLV493D_DATA_Z_GET(buff);
	*t = TLV493D_DATA_TEMP_GET(buff);

out:
	pm_runtime_mark_last_busy(data->dev);
	pm_runtime_put_autosuspend(data->dev);
	return ret;
}

static int tlv493d_init(struct tlv493d_data *data)
{
	int ret;
	u8 buff[TLV493D_RD_REG_MAX];

	if (!data)
		return -EINVAL;

	/*
	 * The sensor initialization requires below steps to be followed,
	 * 1. Power-up sensor.
	 * 2. Read and store read-registers map (0x0-0x9).
	 * 3. Copy values from read reserved registers to write reserved fields (0x0-0x3).
	 * 4. Set operating mode.
	 * 5. Write to all registers.
	 */
	ret = regmap_bulk_read(data->map, TLV493D_RD_REG_BX, buff, ARRAY_SIZE(buff));
	if (ret) {
		dev_err(data->dev, "bulk read failed, error %d\n", ret);
		return ret;
	}

	data->wr_regs[0] = 0; /* Write register 0x0 is reserved. Does not require to be updated.*/
	data->wr_regs[1] = buff[TLV493D_RD_REG_RES1] & TLV493D_RD_REG_RES1_WR_MASK;
	data->wr_regs[2] = buff[TLV493D_RD_REG_RES2] & TLV493D_RD_REG_RES2_WR_MASK;
	data->wr_regs[3] = buff[TLV493D_RD_REG_RES3] & TLV493D_RD_REG_RES3_WR_MASK;

	ret = tlv493d_set_operating_mode(data, data->mode);
	if (ret < 0) {
		dev_err(data->dev, "failed to set operating mode\n");
		return ret;
	}

	return ret;
}

static int tlv493d_parse_dt(struct tlv493d_data *data)
{
	struct device_node *node;
	u32 val = 0;
	int ret;

	if (!data)
		return -EINVAL;

	node = data->dev->of_node;

	/* Optional 'mode' property to set sensor operation mode */
	ret = of_property_read_u32(node, "mode", &val);
	if (ret < 0 || val >= TLV493D_OP_MODE_MAX) {
		/* Fallback to default mode if property is missing or invalid */
		data->mode = TLV493D_OP_MODE_MASTERCONTROLLED;
	} else {
		data->mode = val;
	}

	/*
	 * Read temperature offset (raw value at 25°C). If not specified,
	 * default to 340.
	 */
	ret = of_property_read_u32(node, "temp-offset", &val);
	if (ret)
		val = 340;
	/*
	 * The above is a raw offset; however, IIO expects a single effective offset.
	 * Since final temperature includes an additional fixed 25°C (i.e. 25000 m°C),
	 * we compute a combined offset using scale = 1100 (1.1 * 1000).
	 */
	data->temp_offset = -val + (s32)div_u64(25000, 1100);

	return 0;
}

static int tlv493d_read_raw(struct iio_dev *indio_dev, const struct iio_chan_spec *chan,
		int *val, int *val2, long mask)
{
	struct tlv493d_data *data = iio_priv(indio_dev);
	s16 x, y, z, t;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
	case IIO_CHAN_INFO_RAW:
		ret = tlv493d_get_measurements(data, &x, &y, &z, &t);
		if (ret) {
			dev_err(data->dev, "failed to read sensor data\n");
			return ret;
		}
		/* Return raw values for requested channel */
		switch (chan->address) {
		case AXIS_X:
			*val = x;
			return IIO_VAL_INT;
		case AXIS_Y:
			*val = y;
			return IIO_VAL_INT;
		case AXIS_Z:
			*val = z;
			return IIO_VAL_INT;
		case TEMPERATURE:
			*val = t;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_MAGN:
			/*
			 * Magnetic field scale: 0.0098 mTesla (i.e. 9.8 µT)
			 * Expressed as fractional: 98/10 = 9.8 µT.
			 */
			*val = 98;
			*val2 = 10;
			return IIO_VAL_FRACTIONAL;
		case IIO_TEMP:
			/*
			 * Temperature scale: 1.1 °C per LSB, expressed as 1100 m°C
			 * Returned as integer for IIO core to apply:
			 * temp = (raw + offset) * scale
			 */
			*val = 1.1 * MILLI;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_OFFSET:
		switch (chan->type) {
		case IIO_TEMP:
			/*
			 * Temperature offset includes sensor-specific raw offset
			 * plus compensation for +25°C bias in formula.
			 * This value is precomputed during probe/init:
			 * offset = -raw_offset + (25000 / scale)
			 */
			*val = data->temp_offset;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}

	return 0;
}

static irqreturn_t tlv493d_trigger_handler(int irq, void *ptr)
{
	struct iio_poll_func *pf = ptr;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct tlv493d_data *data = iio_priv(indio_dev);

	struct {
		s16 channels[3];
		s16 temperature;
		aligned_s64 timestamp;
	} scan;

	s16 x, y, z, t;
	int ret;

	ret = tlv493d_get_measurements(data, &x, &y, &z, &t);
	if (ret) {
		dev_err(data->dev, "failed to read sensor data\n");
		goto trig_out;
	}

	scan.channels[0] = x;
	scan.channels[1] = y;
	scan.channels[2] = z;
	scan.temperature = t;

	iio_push_to_buffers_with_ts(indio_dev, &scan, sizeof(scan), pf->timestamp);

trig_out:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

#define TLV493D_AXIS_CHANNEL(axis, index)			\
	{							\
		.type = IIO_MAGN,				\
		.modified = 1,					\
		.channel2 = IIO_MOD_##axis,			\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |	\
				BIT(IIO_CHAN_INFO_SCALE),	\
		.address = index,				\
		.scan_index = index,				\
		.scan_type = {					\
			.sign = 's',				\
			.realbits = 12,				\
			.storagebits = 16,			\
			.endianness = IIO_CPU,			\
		},						\
	}

static const struct iio_chan_spec tlv493d_channels[] = {
	TLV493D_AXIS_CHANNEL(X, AXIS_X),
	TLV493D_AXIS_CHANNEL(Y, AXIS_Y),
	TLV493D_AXIS_CHANNEL(Z, AXIS_Z),
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_SCALE) |
				BIT(IIO_CHAN_INFO_OFFSET),
		.address = TEMPERATURE,
		.scan_index = TEMPERATURE,
		.scan_type = {
			.sign = 's',
			.realbits = 12,
			.storagebits = 16,
			.endianness = IIO_CPU,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(4),
};

static const struct regmap_range tlv493d_volatile_reg_ranges[] = {
	regmap_reg_range(TLV493D_RD_REG_BX, TLV493D_RD_REG_RES3),
};

static const struct regmap_access_table tlv493d_volatile_regs = {
	.yes_ranges = tlv493d_volatile_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(tlv493d_volatile_reg_ranges),
};

static const struct iio_info tlv493d_info = {
	.read_raw = tlv493d_read_raw,
};

static const struct iio_buffer_setup_ops tlv493d_setup_ops = {};

static const unsigned long tlv493d_scan_masks[] = { GENMASK(3, 0), 0 };

static const struct regmap_config tlv493d_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TLV493D_RD_REG_RES3,
	.volatile_table = &tlv493d_volatile_regs,
};

static int tlv493d_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct tlv493d_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->dev = dev;
	data->client = client;
	i2c_set_clientdata(client, indio_dev);

	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;

	data->map = devm_regmap_init_i2c(client, &tlv493d_regmap_config);
	if (IS_ERR(data->map))
		return dev_err_probe(dev, PTR_ERR(data->map), "failed to allocate register map\n");

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable regulator\n");

	ret = tlv493d_parse_dt(data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse device-tree\n");

	ret = tlv493d_init(data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialized\n");

	indio_dev->info = &tlv493d_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->name = client->name;
	indio_dev->channels = tlv493d_channels;
	indio_dev->num_channels = ARRAY_SIZE(tlv493d_channels);
	indio_dev->available_scan_masks = tlv493d_scan_masks;

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
			iio_pollfunc_store_time, tlv493d_trigger_handler,
			&tlv493d_setup_ops);
	if (ret < 0)
		return dev_err_probe(dev, ret, "iio triggered buffer setup failed\n");

	ret = pm_runtime_set_active(dev);
	if (ret < 0)
		return ret;

	ret = devm_pm_runtime_enable(dev);
	if (ret < 0)
		return ret;

	pm_runtime_get_noresume(dev);
	pm_runtime_set_autosuspend_delay(dev, 500);
	pm_runtime_use_autosuspend(dev);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	ret =  devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "iio device register failed\n");

	return 0;
}

static int tlv493d_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tlv493d_data *data = iio_priv(indio_dev);

	return tlv493d_set_operating_mode(data, TLV493D_OP_MODE_POWERDOWN);
}

static int tlv493d_runtime_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tlv493d_data *data = iio_priv(indio_dev);

	return tlv493d_set_operating_mode(data, data->mode);
}

static DEFINE_RUNTIME_DEV_PM_OPS(tlv493d_pm_ops,
		tlv493d_runtime_suspend, tlv493d_runtime_resume, NULL);

static const struct i2c_device_id tlv493d_id[] = {
	{ "tlv493d" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tlv493d_id);

static const struct of_device_id tlv493d_of_match[] = {
	{ .compatible = "infineon,tlv493d-a1b6", },
	{ }
};
MODULE_DEVICE_TABLE(of, tlv493d_of_match);

static struct i2c_driver tlv493d_driver = {
	.driver = {
		.name = "tlv493d",
		.of_match_table = tlv493d_of_match,
		.pm = pm_ptr(&tlv493d_pm_ops),
	},
	.probe = tlv493d_probe,
	.id_table = tlv493d_id,
};

module_i2c_driver(tlv493d_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Infineon TLV493D Low-Power 3D Magnetic Sensor");
MODULE_AUTHOR("Dixit Parmar <dixitparmar19@gmail.com>");
