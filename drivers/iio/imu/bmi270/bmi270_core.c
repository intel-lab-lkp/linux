// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)

#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "bmi270.h"

#define BMI270_CHIP_ID 0x24
#define BMI270_INIT_DATA_FILE "bmi270-init-data.fw"

enum bmi270_registers {
	BMI270_REG_CHIP_ID = 0x00,
	BMI270_REG_INTERNAL_STATUS = 0x21,
	BMI270_REG_ACC_CONF = 0x40,
	BMI270_REG_GYR_CONF = 0x42,
	BMI270_REG_INIT_CTRL = 0x59,
	BMI270_REG_INIT_DATA = 0x5e,
	BMI270_REG_PWR_CONF = 0x7c,
	BMI270_REG_PWR_CTRL = 0x7d,
};

enum bmi270_scan {
	BMI270_SCAN_ACCEL_X,
	BMI270_SCAN_ACCEL_Y,
	BMI270_SCAN_ACCEL_Z,
	BMI270_SCAN_GYRO_X,
	BMI270_SCAN_GYRO_Y,
	BMI270_SCAN_GYRO_Z,
};

const struct regmap_config bmi270_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};
EXPORT_SYMBOL_NS_GPL(bmi270_regmap_config, IIO_BMI270);

static int bmi270_get_data(struct bmi270_data *bmi270_device,
			   int chan_type, int axis, int *val)
{
	__le16 sample;
	int reg;

	switch (chan_type) {
	case IIO_ACCEL:
		reg = 0xc + (axis - IIO_MOD_X) * sizeof(sample);
		break;
	case IIO_ANGL_VEL:
		reg = 0x12 + (axis - IIO_MOD_X) * sizeof(sample);
		break;
	default:
		return -EINVAL;
	}

	regmap_bulk_read(bmi270_device->regmap, reg, &sample, sizeof(sample));
	*val = sign_extend32(le16_to_cpu(sample), 15);

	return 0;
}

static int bmi270_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct bmi270_data *bmi270_device = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		bmi270_get_data(bmi270_device, chan->type, chan->channel2, val);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct iio_info bmi270_info = {
	.read_raw = bmi270_read_raw,
};

static const struct iio_chan_spec bmi270_channels[] = {
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
			BIT(IIO_CHAN_INFO_FREQUENCY),
		.scan_index = BMI270_SCAN_ACCEL_X,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
			BIT(IIO_CHAN_INFO_FREQUENCY),
		.scan_index = BMI270_SCAN_ACCEL_Y,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
			BIT(IIO_CHAN_INFO_FREQUENCY),
		.scan_index = BMI270_SCAN_ACCEL_Z,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
	{
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
			BIT(IIO_CHAN_INFO_FREQUENCY),
		.scan_index = BMI270_SCAN_GYRO_X,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
	{
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
			BIT(IIO_CHAN_INFO_FREQUENCY),
		.scan_index = BMI270_SCAN_GYRO_Y,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},

	},
	{
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
			BIT(IIO_CHAN_INFO_FREQUENCY),
		.scan_index = BMI270_SCAN_GYRO_Z,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
};

static int bmi270_validate_chip_id(struct bmi270_data *bmi270_device)
{
	int chip_id;
	int ret;
	struct device *dev = bmi270_device->dev;
	struct regmap *regmap = bmi270_device->regmap;

	ret = regmap_read(regmap, BMI270_REG_CHIP_ID, &chip_id);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read chip id");

	if (chip_id != BMI270_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "Invalid chip id");

	return 0;
}

static int bmi270_write_init_data(struct bmi270_data *bmi270_device)
{
	int pwr_conf = 0;
	int ret;
	int status = 0;
	const struct firmware *init_data;
	struct device *dev = bmi270_device->dev;
	struct regmap *regmap = bmi270_device->regmap;

	ret = regmap_read(regmap, BMI270_REG_PWR_CONF, &pwr_conf);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read power configuration");

	pwr_conf &=  0xfffffffe;
	ret = regmap_write(regmap, BMI270_REG_PWR_CONF, pwr_conf);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write power configuration");

	usleep_range(450, 1000);

	ret = regmap_write(regmap, BMI270_REG_INIT_CTRL, 0x0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to prepare device to load init data");

	ret = request_firmware(&init_data, BMI270_INIT_DATA_FILE, dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to load init data file");

	ret = regmap_bulk_write(regmap, BMI270_REG_INIT_DATA,
				init_data->data, init_data->size);
	release_firmware(init_data);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write init data");

	ret = regmap_write(regmap, BMI270_REG_INIT_CTRL, 0x1);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to stop device initialization");

	usleep_range(20000, 55000);

	ret = regmap_read(regmap, BMI270_REG_INTERNAL_STATUS, &status);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read internal status");

	if (status != 1)
		return dev_err_probe(dev, -ENODEV, "Device failed to initialize");

	return 0;
}

static int bmi270_configure_imu(struct bmi270_data *bmi270_device)
{
	int ret;
	struct device *dev = bmi270_device->dev;
	struct regmap *regmap = bmi270_device->regmap;

	ret = regmap_write(regmap, BMI270_REG_PWR_CTRL, 0x0e);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable accelerometer and gyroscope");

	ret = regmap_write(regmap, BMI270_REG_ACC_CONF, 0xa8);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure accelerometer");

	ret = regmap_write(regmap, BMI270_REG_GYR_CONF, 0xa9);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure gyroscope");

	ret = regmap_write(regmap, BMI270_REG_PWR_CONF, 0x02);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set power configuration");

	return 0;
}

static int bmi270_chip_init(struct bmi270_data *bmi270_device)
{
	int ret;

	ret = bmi270_validate_chip_id(bmi270_device);
	if (ret)
		return ret;

	ret = bmi270_write_init_data(bmi270_device);
	if (ret)
		return ret;

	ret = bmi270_configure_imu(bmi270_device);
	if (ret)
		return ret;

	return 0;
}

int bmi270_core_probe(struct device *dev, struct regmap *regmap,
		      const char *name, bool use_spi)
{
	int ret;
	struct bmi270_data *bmi270_device;
	struct iio_dev *indio_dev;

	indio_dev = devm_iio_device_alloc(dev, sizeof(struct bmi270_data *));
	if (!indio_dev)
		return -ENOMEM;

	bmi270_device = iio_priv(indio_dev);
	bmi270_device->dev = dev;
	bmi270_device->regmap = regmap;

	dev_set_drvdata(dev, indio_dev);

	ret = bmi270_chip_init(bmi270_device);
	if (ret)
		return ret;

	indio_dev->channels = bmi270_channels;
	indio_dev->num_channels = ARRAY_SIZE(bmi270_channels);
	indio_dev->name = name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &bmi270_info;

	return devm_iio_device_register(dev, indio_dev);
}
EXPORT_SYMBOL_NS_GPL(bmi270_core_probe, IIO_BMI270);

MODULE_AUTHOR("Alex Lanzano");
MODULE_DESCRIPTION("BMI270 driver");
MODULE_LICENSE("GPL");
