// SPDX-License-Identifier: GPL-2.0-only
/*
 * qmc5883p.c - QMC5883P magnetometer driver
 *
 * Copyright 2026 Hardik Phalet <hardik.phalet@pm.me>
 *
 * TODO: add triggered buffer support, PM, OSR, DSR
 *
 */

#include <linux/array_size.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <linux/unaligned.h>

/*
 * Register definition
 */
#define QMC5883P_REG_CHIP_ID 0x00
#define QMC5883P_REG_X_LSB 0x01
#define QMC5883P_REG_X_MSB 0x02
#define QMC5883P_REG_Y_LSB 0x03
#define QMC5883P_REG_Y_MSB 0x04
#define QMC5883P_REG_Z_LSB 0x05
#define QMC5883P_REG_Z_MSB 0x06
#define QMC5883P_REG_STATUS 0x09
#define QMC5883P_REG_CTRL_1 0x0A
#define QMC5883P_REG_CTRL_2 0x0B

/*
 * Value definition
 */
#define QMC5883P_MODE_SUSPEND 0x00
#define QMC5883P_MODE_NORMAL 0x01
#define QMC5883P_MODE_SINGLE 0x02
#define QMC5883P_MODE_CONTINUOUS 0x03

/*
 * Output data rate
 */
#define QMC5883P_ODR_10 0x00
#define QMC5883P_ODR_50 0x01
#define QMC5883P_ODR_100 0x02
#define QMC5883P_ODR_200 0x03

/*
 * Oversampling rate
 */
#define QMC5883P_OSR_8 0x00
#define QMC5883P_OSR_4 0x01
#define QMC5883P_OSR_2 0x02
#define QMC5883P_OSR_1 0x03

#define QMC5883P_RNG_30G 0x00
#define QMC5883P_RNG_12G 0x01
#define QMC5883P_RNG_08G 0x02
#define QMC5883P_RNG_02G 0x03

#define QMC5883P_DRDY_POLL_US 1000

#define QMC5883P_CHIP_ID 0x80

#define QMC5883P_STATUS_DRDY BIT(0)
#define QMC5883P_STATUS_OVFL BIT(1)

struct qmc5883p_rf {
	struct regmap_field *osr;
	struct regmap_field *odr;
	struct regmap_field *mode;
	struct regmap_field *rng;
	struct regmap_field *sftrst;
	struct regmap_field *chip_id;
};

struct qmc5883p_data {
	struct device *dev;
	struct regmap *regmap;
	struct mutex mutex; /* protects regmap and rf field accesses */
	struct qmc5883p_rf rf;
};

enum qmc5883p_channels {
	AXIS_X = 0,
	AXIS_Y,
	AXIS_Z,
};

/*
 * Scale factors in nT/LSB for IIO_VAL_INT_PLUS_NANO, derived from datasheet
 * Table 2 sensitivities (LSB/G) converted to LSB/T (1 G = 1e-4 T):
 *   sensitivity_T = sensitivity_G * 10000
 *   scale_nT     = 1e9 / sensitivity_T
 *
 * The 8G and 2G entries truncate 26.666... and 6.666... nT/LSB respectively;
 * IIO_VAL_INT_PLUS_NANO cannot carry the exact rationals, but the chosen
 * values match what IIO_VAL_FRACTIONAL would have rendered and therefore
 * round-trip cleanly through sysfs write back.
 *
 * Index matches register value: RNG<1:0> = 0b00..0b11
 */
static const int qmc5883p_scale[][2] = {
	[QMC5883P_RNG_30G] = { 0, 100 },
	[QMC5883P_RNG_12G] = { 0, 40 },
	[QMC5883P_RNG_08G] = { 0, 26 },
	[QMC5883P_RNG_02G] = { 0, 6 },
};

static const int qmc5883p_odr[] = {
	[QMC5883P_ODR_10] = 10,
	[QMC5883P_ODR_50] = 50,
	[QMC5883P_ODR_100] = 100,
	[QMC5883P_ODR_200] = 200,
};

static const struct regmap_range qmc5883p_readable_ranges[] = {
	regmap_reg_range(QMC5883P_REG_CHIP_ID, QMC5883P_REG_Z_MSB),
	regmap_reg_range(QMC5883P_REG_STATUS, QMC5883P_REG_CTRL_2),
};

static const struct regmap_range qmc5883p_writable_ranges[] = {
	regmap_reg_range(QMC5883P_REG_CTRL_1, QMC5883P_REG_CTRL_2),
};

/*
 * Volatile registers: hardware updates these independently of the driver.
 * regmap will never serve these from cache.
 */
static const struct regmap_range qmc5883p_volatile_ranges[] = {
	regmap_reg_range(QMC5883P_REG_X_LSB, QMC5883P_REG_Z_MSB),
	regmap_reg_range(QMC5883P_REG_STATUS, QMC5883P_REG_STATUS),
	regmap_reg_range(QMC5883P_REG_CTRL_2, QMC5883P_REG_CTRL_2),
};

/*
 * Precious registers: reading has a side effect (clears DRDY/OVFL bits).
 * regmap will never read these speculatively.
 */
static const struct regmap_range qmc5883p_precious_ranges[] = {
	regmap_reg_range(QMC5883P_REG_STATUS, QMC5883P_REG_STATUS),
};

static const struct regmap_access_table qmc5883p_readable_table = {
	.yes_ranges = qmc5883p_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(qmc5883p_readable_ranges),
};

static const struct regmap_access_table qmc5883p_writable_table = {
	.yes_ranges = qmc5883p_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(qmc5883p_writable_ranges),
};

static const struct regmap_access_table qmc5883p_volatile_table = {
	.yes_ranges = qmc5883p_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(qmc5883p_volatile_ranges),
};

static const struct regmap_access_table qmc5883p_precious_table = {
	.yes_ranges = qmc5883p_precious_ranges,
	.n_yes_ranges = ARRAY_SIZE(qmc5883p_precious_ranges),
};

static const struct regmap_config qmc5883p_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x0B,
	.cache_type = REGCACHE_RBTREE,
	.rd_table = &qmc5883p_readable_table,
	.wr_table = &qmc5883p_writable_table,
	.volatile_table = &qmc5883p_volatile_table,
	.precious_table = &qmc5883p_precious_table,
};

static const struct reg_field qmc5883p_rf_osr =
	REG_FIELD(QMC5883P_REG_CTRL_1, 4, 5);
static const struct reg_field qmc5883p_rf_odr =
	REG_FIELD(QMC5883P_REG_CTRL_1, 2, 3);
static const struct reg_field qmc5883p_rf_mode =
	REG_FIELD(QMC5883P_REG_CTRL_1, 0, 1);
static const struct reg_field qmc5883p_rf_rng =
	REG_FIELD(QMC5883P_REG_CTRL_2, 2, 3);
static const struct reg_field qmc5883p_rf_sftrst =
	REG_FIELD(QMC5883P_REG_CTRL_2, 7, 7);
static const struct reg_field qmc5883p_rf_chip_id =
	REG_FIELD(QMC5883P_REG_CHIP_ID, 0, 7);

/*
 * qmc5883p_get_measure - read all three axes.
 * Must be called with data->mutex held.
 */
static int qmc5883p_get_measure(struct qmc5883p_data *data, s16 *x, s16 *y,
				s16 *z)
{
	int ret;
	u8 reg_data[6];
	unsigned int status;

	/*
	 * Poll the status register until DRDY is set or timeout.
	 * Read the whole register in one shot so that OVFL is captured from
	 * the same read: reading 0x09 clears both DRDY and OVFL, so a second
	 * read would always see OVFL=0.
	 * At ODR=10Hz one period is 100ms; use 150ms as a safe upper bound.
	 */
	ret = regmap_read_poll_timeout(data->regmap, QMC5883P_REG_STATUS,
				       status, status & QMC5883P_STATUS_DRDY,
				       QMC5883P_DRDY_POLL_US,
				       150 * (MICRO / MILLI));
	if (ret)
		return ret;

	if (status & QMC5883P_STATUS_OVFL) {
		dev_warn_ratelimited(data->dev,
			"data overflow, consider reducing field range\n");
		ret = -ERANGE;
		return ret;
	}

	ret = regmap_bulk_read(data->regmap, QMC5883P_REG_X_LSB, reg_data,
			       ARRAY_SIZE(reg_data));
	if (ret)
		return ret;

	*x = (s16)get_unaligned_le16(&reg_data[0]);
	*y = (s16)get_unaligned_le16(&reg_data[2]);
	*z = (s16)get_unaligned_le16(&reg_data[4]);

	return ret;
}

static int qmc5883p_read_raw(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan, int *val,
			     int *val2, long mask)
{
	s16 x, y, z;
	struct qmc5883p_data *data = iio_priv(indio_dev);
	int ret;
	unsigned int regval;

	guard(mutex)(&data->mutex);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = qmc5883p_get_measure(data, &x, &y, &z);
		if (ret < 0)
			return ret;
		switch (chan->address) {
		case AXIS_X:
			*val = x;
			break;
		case AXIS_Y:
			*val = y;
			break;
		case AXIS_Z:
			*val = z;
			break;
		}
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		ret = regmap_field_read(data->rf.rng, &regval);
		if (ret < 0)
			return ret;
		*val = qmc5883p_scale[regval][0];
		*val2 = qmc5883p_scale[regval][1];
		return IIO_VAL_INT_PLUS_NANO;

	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = regmap_field_read(data->rf.odr, &regval);
		if (ret < 0)
			return ret;
		*val = qmc5883p_odr[regval];
		return IIO_VAL_INT;
	}

	return -EINVAL;
}

static int qmc5883p_write_scale(struct qmc5883p_data *data, int val, int val2)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(qmc5883p_scale); i++) {
		if (qmc5883p_scale[i][0] == val && qmc5883p_scale[i][1] == val2)
			return regmap_field_write(data->rf.rng, i);
	}

	return -EINVAL;
}

static int qmc5883p_write_odr(struct qmc5883p_data *data, int val)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(qmc5883p_odr); i++) {
		if (qmc5883p_odr[i] == val)
			return regmap_field_write(data->rf.odr, i);
	}

	return -EINVAL;
}

static int qmc5883p_write_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan, int val,
			      int val2, long mask)
{
	struct qmc5883p_data *data = iio_priv(indio_dev);
	int ret, restore;

	guard(mutex)(&data->mutex);

	ret = regmap_field_write(data->rf.mode, QMC5883P_MODE_SUSPEND);
	if (ret)
		return ret;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = qmc5883p_write_odr(data, val);
		break;
	case IIO_CHAN_INFO_SCALE:
		ret = qmc5883p_write_scale(data, val, val2);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	restore = regmap_field_write(data->rf.mode, QMC5883P_MODE_NORMAL);
	if (restore && !ret)
		ret = restore;

	return ret;
}

/*
 * qmc5883p_read_avail - expose available values to userspace.
 *
 * Creates the _available sysfs attributes automatically:
 *   in_magn_sampling_frequency_available
 *   in_magn_scale_available
 */
static int qmc5883p_read_avail(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       const int **vals, int *type, int *length,
			       long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = qmc5883p_odr;
		*type = IIO_VAL_INT;
		*length = ARRAY_SIZE(qmc5883p_odr);
		return IIO_AVAIL_LIST;

	case IIO_CHAN_INFO_SCALE:
		*vals = (const int *)qmc5883p_scale;
		*type = IIO_VAL_INT_PLUS_NANO;
		*length = ARRAY_SIZE(qmc5883p_scale) * 2;
		return IIO_AVAIL_LIST;

	default:
		return -EINVAL;
	}
}

/*
 * Tell the IIO core how to parse sysfs writes. Without this, the core
 * defaults to IIO_VAL_INT_PLUS_MICRO (6 fractional digits), which would
 * silently truncate nano-scale writes like "0.000000040" to 0.
 */
static int qmc5883p_write_raw_get_fmt(struct iio_dev *indio_dev,
				      struct iio_chan_spec const *chan,
				      long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info qmc5883p_info = {
	.read_raw = qmc5883p_read_raw,
	.write_raw = qmc5883p_write_raw,
	.write_raw_get_fmt = qmc5883p_write_raw_get_fmt,
	.read_avail = qmc5883p_read_avail,
};

static int qmc5883p_rf_init(struct qmc5883p_data *data)
{
	struct regmap *regmap = data->regmap;
	struct device *dev = data->dev;
	struct qmc5883p_rf *rf = &data->rf;

	rf->osr = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_osr);
	if (IS_ERR(rf->osr))
		return PTR_ERR(rf->osr);

	rf->odr = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_odr);
	if (IS_ERR(rf->odr))
		return PTR_ERR(rf->odr);

	rf->mode = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_mode);
	if (IS_ERR(rf->mode))
		return PTR_ERR(rf->mode);

	rf->rng = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_rng);
	if (IS_ERR(rf->rng))
		return PTR_ERR(rf->rng);

	rf->sftrst = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_sftrst);
	if (IS_ERR(rf->sftrst))
		return PTR_ERR(rf->sftrst);

	rf->chip_id = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_chip_id);
	if (IS_ERR(rf->chip_id))
		return PTR_ERR(rf->chip_id);

	return 0;
}

static int qmc5883p_read_chip_id(struct qmc5883p_data *data)
{
	int ret, regval;

	ret = regmap_field_read(data->rf.chip_id, &regval);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "failed to read chip ID\n");

	if (regval != QMC5883P_CHIP_ID)
		dev_info(data->dev, "unexpected chip ID %#x, expected %#x\n",
			regval, QMC5883P_CHIP_ID);

	return 0;
}

#define QMC5883P_CHAN(ch)                                                 \
	{                                                                 \
		.type = IIO_MAGN,                                         \
		.channel2 = IIO_MOD_##ch,                                 \
		.modified = 1,                                            \
		.address = AXIS_##ch,                                     \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |            \
				      BIT(IIO_CHAN_INFO_SCALE),           \
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE), \
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.info_mask_shared_by_type_available =                     \
			BIT(IIO_CHAN_INFO_SAMP_FREQ),                     \
	}

static const struct iio_chan_spec qmc5883p_channels[] = {
	QMC5883P_CHAN(X),
	QMC5883P_CHAN(Y),
	QMC5883P_CHAN(Z),
};

static int qmc5883p_chip_init(struct qmc5883p_data *data)
{
	int ret;

	ret = regmap_field_write(data->rf.sftrst, 1);
	if (ret)
		return ret;

	/*
	 * The datasheet does not specify a post-reset delay, but POR
	 * completion takes up to 250 microseconds. Use 300 microseconds
	 * to be safe.
	 */
	fsleep(300);

	ret = regmap_field_write(data->rf.sftrst, 0);
	if (ret)
		return ret;

	/*
	 * Soft reset restored every register to its default. Drop the cache
	 * so subsequent RMW writes read fresh values from the device.
	 */
	regcache_drop_region(data->regmap, QMC5883P_REG_CHIP_ID,
			     QMC5883P_REG_CTRL_2);

	/* Chip is now in MODE_SUSPEND per datasheet §6.2.4. Leave it there. */
	return 0;
}

static int qmc5883p_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct qmc5883p_data *data;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	regmap = devm_regmap_init_i2c(client, &qmc5883p_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "regmap initialization failed\n");

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->dev = dev;
	data->regmap = regmap;

	mutex_init(&data->mutex);

	ret = qmc5883p_rf_init(data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize regmap fields\n");

	indio_dev->name = "qmc5883p";
	indio_dev->info = &qmc5883p_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = qmc5883p_channels;
	indio_dev->num_channels = ARRAY_SIZE(qmc5883p_channels);

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize vdd regulator\n");

	/* Datasheet specifies up to 50 ms supply ramp + 250 us POR time. */
	fsleep(50 * (MICRO / MILLI) + 250);

	ret = qmc5883p_read_chip_id(data);
	if (ret)
		return ret;

	ret = qmc5883p_chip_init(data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize chip\n");

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id qmc5883p_of_match[] = {
	{ .compatible = "qstcorp,qmc5883p" },
	{ }
};
MODULE_DEVICE_TABLE(of, qmc5883p_of_match);

static const struct i2c_device_id qmc5883p_id[] = {
	{ "qmc5883p", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, qmc5883p_id);

static struct i2c_driver qmc5883p_driver = {
	.driver = {
		.name = "qmc5883p",
		.of_match_table = qmc5883p_of_match,
	},
	.probe = qmc5883p_probe,
	.id_table = qmc5883p_id,
};
module_i2c_driver(qmc5883p_driver);

MODULE_AUTHOR("Hardik Phalet <hardik.phalet@pm.me>");
MODULE_DESCRIPTION("QST QMC5883P 3-axis magnetometer driver");
MODULE_LICENSE("GPL");
