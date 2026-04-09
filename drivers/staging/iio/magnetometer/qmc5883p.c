// SPDX-License-Identifier: GPL-2.0-only
/*
 * qmc5883p.c - QMC5883P magnetometer driver
 *
 * Copyright 2026 Hardik Phalet <hardik.phalet@pm.me>
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/types.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

/* Register definition */
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

/* Value definition */
#define QMC5883P_MODE_SUSPEND 0x00
#define QMC5883P_MODE_NORMAL 0x01
#define QMC5883P_MODE_SINGLE 0x02
#define QMC5883P_MODE_CONTINUOUS 0x03

/* Output data rate */
#define QMC5883P_ODR_10 0x00
#define QMC5883P_ODR_50 0x01
#define QMC5883P_ODR_100 0x02
#define QMC5883P_ODR_200 0x03

/* Oversampling rate */
#define QMC5883P_OSR_8 0x00
#define QMC5883P_OSR_4 0x01
#define QMC5883P_OSR_2 0x02
#define QMC5883P_OSR_1 0x03

/* Downsampling rate */
#define QMC5883P_DSR_1 0x00
#define QMC5883P_DSR_2 0x01
#define QMC5883P_DSR_4 0x02
#define QMC5883P_DSR_8 0x03

#define QMC5883P_RSTCTRL_SET_RESET \
	0x00 /* Set and reset on, i.e. the offset of device is renewed */
#define QMC5883P_RSTCTRL_SET_ONLY 0x01 /* Set only on */
#define QMC5883P_RSTCTRL_OFF 0x02 /* Set and reset off */

#define QMC5883P_RNG_30G 0x00
#define QMC5883P_RNG_12G 0x01
#define QMC5883P_RNG_08G 0x02
#define QMC5883P_RNG_02G 0x03

#define QMC5883P_DEFAULT_ODR QMC5883P_ODR_100
#define QMC5883P_DEFAULT_OSR QMC5883P_OSR_4
#define QMC5883P_DEFAULT_DSR QMC5883P_DSR_4
#define QMC5883P_DEFAULT_RNG QMC5883P_RNG_08G

#define QMC5883P_DRDY_POLL_US 1000

#define QMC5883P_CHIP_ID 0x80

#define QMC5883P_STATUS_DRDY BIT(0)
#define QMC5883P_STATUS_OVFL BIT(1)

/*
 * Scale factors in T/LSB for IIO_VAL_FRACTIONAL (val/val2), derived from
 * datasheet Table 2 sensitivities (LSB/G) converted to LSB/T (1 G = 1e-4 T):
 *   sensitivity_T = sensitivity_G * 10000
 *   scale = 1 / sensitivity_T
 *
 * Index matches register value: RNG<1:0> = 0b00..0b11
 */
static const int qmc5883p_scale[][2] = {
	[QMC5883P_RNG_30G] = { 1, 10000000 },
	[QMC5883P_RNG_12G] = { 1, 25000000 },
	[QMC5883P_RNG_08G] = { 1, 37500000 },
	[QMC5883P_RNG_02G] = { 1, 150000000 },
};

static const int qmc5883p_odr[] = {
	[QMC5883P_ODR_10] = 10,
	[QMC5883P_ODR_50] = 50,
	[QMC5883P_ODR_100] = 100,
	[QMC5883P_ODR_200] = 200,
};

static const int qmc5883p_osr[] = {
	[QMC5883P_OSR_1] = 1,
	[QMC5883P_OSR_2] = 2,
	[QMC5883P_OSR_4] = 4,
	[QMC5883P_OSR_8] = 8,
};

static const unsigned int qmc5883p_dsr[] = {
	[QMC5883P_DSR_1] = 1,
	[QMC5883P_DSR_2] = 2,
	[QMC5883P_DSR_4] = 4,
	[QMC5883P_DSR_8] = 8,
};

struct qmc5883p_rf {
	struct regmap_field *osr;
	struct regmap_field *dsr;
	struct regmap_field *odr;
	struct regmap_field *mode;
	struct regmap_field *rng;
	struct regmap_field *rstctrl;
	struct regmap_field *sftrst;
	struct regmap_field *selftest;
	struct regmap_field *chip_id;
};

static const struct regmap_range qmc5883p_readable_ranges[] = {
	regmap_reg_range(QMC5883P_REG_CHIP_ID, QMC5883P_REG_STATUS),
	regmap_reg_range(QMC5883P_REG_CTRL_1, QMC5883P_REG_CTRL_2),
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

static const struct reg_field qmc5883p_rf_osr =
	REG_FIELD(QMC5883P_REG_CTRL_1, 4, 5);
static const struct reg_field qmc5883p_rf_dsr =
	REG_FIELD(QMC5883P_REG_CTRL_1, 6, 7);
static const struct reg_field qmc5883p_rf_odr =
	REG_FIELD(QMC5883P_REG_CTRL_1, 2, 3);
static const struct reg_field qmc5883p_rf_mode =
	REG_FIELD(QMC5883P_REG_CTRL_1, 0, 1);
static const struct reg_field qmc5883p_rf_rng =
	REG_FIELD(QMC5883P_REG_CTRL_2, 2, 3);
static const struct reg_field qmc5883p_rf_rstctrl =
	REG_FIELD(QMC5883P_REG_CTRL_2, 0, 1);
static const struct reg_field qmc5883p_rf_sftrst =
	REG_FIELD(QMC5883P_REG_CTRL_2, 7, 7);
static const struct reg_field qmc5883p_rf_selftest =
	REG_FIELD(QMC5883P_REG_CTRL_2, 6, 6);
static const struct reg_field qmc5883p_rf_chip_id =
	REG_FIELD(QMC5883P_REG_CHIP_ID, 0, 7);

static int qmc5883p_rf_init(struct qmc5883p_data *data)
{
	struct regmap *regmap = data->regmap;
	struct device *dev = data->dev;
	struct qmc5883p_rf *rf = &data->rf;

	rf->osr = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_osr);
	if (IS_ERR(rf->osr))
		return PTR_ERR(rf->osr);

	rf->dsr = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_dsr);
	if (IS_ERR(rf->dsr))
		return PTR_ERR(rf->dsr);

	rf->odr = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_odr);
	if (IS_ERR(rf->odr))
		return PTR_ERR(rf->odr);

	rf->mode = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_mode);
	if (IS_ERR(rf->mode))
		return PTR_ERR(rf->mode);

	rf->rng = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_rng);
	if (IS_ERR(rf->rng))
		return PTR_ERR(rf->rng);

	rf->rstctrl = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_rstctrl);
	if (IS_ERR(rf->rstctrl))
		return PTR_ERR(rf->rstctrl);

	rf->sftrst = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_sftrst);
	if (IS_ERR(rf->sftrst))
		return PTR_ERR(rf->sftrst);

	rf->selftest =
		devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_selftest);
	if (IS_ERR(rf->selftest))
		return PTR_ERR(rf->selftest);

	rf->chip_id = devm_regmap_field_alloc(dev, regmap, qmc5883p_rf_chip_id);
	if (IS_ERR(rf->chip_id))
		return PTR_ERR(rf->chip_id);

	return 0;
}

static int qmc5883p_verify_chip_id(struct qmc5883p_data *data)
{
	int ret, regval;

	ret = regmap_field_read(data->rf.chip_id, &regval);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "failed to read chip ID\n");

	if (regval != QMC5883P_CHIP_ID)
		return dev_err_probe(data->dev, -ENODEV,
				     "unexpected chip ID 0x%02x, expected 0x%02x\n",
				     regval, QMC5883P_CHIP_ID);
	return ret;
}

static int qmc5883p_chip_init(struct qmc5883p_data *data)
{
	int ret;

	ret = regmap_field_write(data->rf.sftrst, 1);
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	ret = regmap_field_write(data->rf.sftrst, 0);
	if (ret)
		return ret;

	ret = regmap_field_write(data->rf.rstctrl, QMC5883P_RSTCTRL_SET_RESET);
	if (ret)
		return ret;

	ret = regmap_field_write(data->rf.rng, QMC5883P_DEFAULT_RNG);
	if (ret)
		return ret;

	ret = regmap_field_write(data->rf.osr, QMC5883P_DEFAULT_OSR);
	if (ret)
		return ret;

	ret = regmap_field_write(data->rf.dsr, QMC5883P_DEFAULT_DSR);
	if (ret)
		return ret;

	ret = regmap_field_write(data->rf.odr, QMC5883P_DEFAULT_ODR);
	if (ret)
		return ret;

	return regmap_field_write(data->rf.mode, QMC5883P_MODE_NORMAL);
}

/*
 * qmc5883p_get_measure - read all three axes.
 * Must be called with data->mutex held.
 * Handles PM internally: resumes device, reads data, schedules autosuspend.
 */
static int qmc5883p_get_measure(struct qmc5883p_data *data, s16 *x, s16 *y,
				s16 *z)
{
	int ret;
	u8 reg_data[6];
	unsigned int status;

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return ret;

	/*
	 * Poll the status register until DRDY is set or timeout.
	 * Read the whole register in one shot so that OVFL is captured from
	 * the same read: reading 0x09 clears both DRDY and OVFL, so a second
	 * read would always see OVFL=0.
	 * At ODR=10Hz one period is 100ms; use 150ms as a safe upper bound.
	 */
	ret = regmap_read_poll_timeout(data->regmap, QMC5883P_REG_STATUS,
				       status, status & QMC5883P_STATUS_DRDY,
				       QMC5883P_DRDY_POLL_US, 150000);
	if (ret)
		goto out;

	if (status & QMC5883P_STATUS_OVFL) {
		dev_warn_ratelimited(data->dev,
				     "data overflow, consider reducing field range\n");
		ret = -ERANGE;
		goto out;
	}

	ret = regmap_bulk_read(data->regmap, QMC5883P_REG_X_LSB, reg_data,
			       ARRAY_SIZE(reg_data));
	if (ret)
		goto out;

	*x = (s16)((reg_data[1] << 8) | reg_data[0]);
	*y = (s16)((reg_data[3] << 8) | reg_data[2]);
	*z = (s16)((reg_data[5] << 8) | reg_data[4]);

out:
	pm_runtime_mark_last_busy(data->dev);
	pm_runtime_put_autosuspend(data->dev);
	return ret;
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

static int qmc5883p_write_osr(struct qmc5883p_data *data, int val)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(qmc5883p_osr); i++) {
		if (qmc5883p_osr[i] == val)
			return regmap_field_write(data->rf.osr, i);
	}

	return -EINVAL;
}

static ssize_t downsampling_ratio_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct qmc5883p_data *data = iio_priv(indio_dev);
	unsigned int regval;
	int ret;

	guard(mutex)(&data->mutex);

	ret = regmap_field_read(data->rf.dsr, &regval);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", qmc5883p_dsr[regval]);
}

static ssize_t downsampling_ratio_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct qmc5883p_data *data = iio_priv(indio_dev);
	unsigned int val;
	int i, ret, restore;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	guard(mutex)(&data->mutex);

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = regmap_field_write(data->rf.mode, QMC5883P_MODE_SUSPEND);
	if (ret)
		goto out;

	ret = -EINVAL;
	for (i = 0; i < ARRAY_SIZE(qmc5883p_dsr); i++) {
		if (qmc5883p_dsr[i] == val) {
			ret = regmap_field_write(data->rf.dsr, i);
			break;
		}
	}

	restore = regmap_field_write(data->rf.mode, QMC5883P_MODE_NORMAL);
	if (restore && !ret)
		ret = restore;

out:
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret ? ret : (ssize_t)len;
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
		return IIO_VAL_FRACTIONAL;

	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = regmap_field_read(data->rf.odr, &regval);
		if (ret < 0)
			return ret;
		*val = qmc5883p_odr[regval];
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		ret = regmap_field_read(data->rf.osr, &regval);
		if (ret < 0)
			return ret;
		*val = qmc5883p_osr[regval];
		return IIO_VAL_INT;
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

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret)
		return ret;

	ret = regmap_field_write(data->rf.mode, QMC5883P_MODE_SUSPEND);
	if (ret)
		goto out;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = qmc5883p_write_odr(data, val);
		break;
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		ret = qmc5883p_write_osr(data, val);
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

out:
	pm_runtime_mark_last_busy(data->dev);
	pm_runtime_put_autosuspend(data->dev);
	return ret;
}

/*
 * qmc5883p_read_avail - expose available values to userspace.
 *
 * Creates the _available sysfs attributes automatically:
 *   in_magn_sampling_frequency_available
 *   in_magn_oversampling_ratio_available
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

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		*vals = qmc5883p_osr;
		*type = IIO_VAL_INT;
		*length = ARRAY_SIZE(qmc5883p_osr);
		return IIO_AVAIL_LIST;

	case IIO_CHAN_INFO_SCALE:
		*vals = (const int *)qmc5883p_scale;
		*type = IIO_VAL_FRACTIONAL;
		*length = ARRAY_SIZE(qmc5883p_scale) * 2;
		return IIO_AVAIL_LIST;

	default:
		return -EINVAL;
	}
}

static IIO_DEVICE_ATTR(downsampling_ratio, 0644, downsampling_ratio_show,
		       downsampling_ratio_store, 0);
static IIO_CONST_ATTR(downsampling_ratio_available, "1 2 4 8");

static struct attribute *qmc5883p_attributes[] = {
	&iio_dev_attr_downsampling_ratio.dev_attr.attr,
	&iio_const_attr_downsampling_ratio_available.dev_attr.attr, NULL
};

static const struct attribute_group qmc5883p_attribute_group = {
	.attrs = qmc5883p_attributes,
};

static const struct iio_info qmc5883p_info = {
	.attrs = &qmc5883p_attribute_group,
	.read_raw = qmc5883p_read_raw,
	.write_raw = qmc5883p_write_raw,
	.read_avail = qmc5883p_read_avail,
};

static const struct iio_chan_spec qmc5883p_channels[] = {
	{
		.type = IIO_MAGN,
		.channel2 = IIO_MOD_X,
		.modified = 1,
		.address = AXIS_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_shared_by_type =
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),
		.info_mask_shared_by_type_available =
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),
	},
	{
		.type = IIO_MAGN,
		.channel2 = IIO_MOD_Y,
		.modified = 1,
		.address = AXIS_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_shared_by_type =
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),
		.info_mask_shared_by_type_available =
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),
	},
	{
		.type = IIO_MAGN,
		.channel2 = IIO_MOD_Z,
		.modified = 1,
		.address = AXIS_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_shared_by_type =
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),
		.info_mask_shared_by_type_available =
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),
	},
};

static void qmc5883p_runtime_pm_disable(void *dev)
{
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
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
	data->dev = dev;
	data->regmap = regmap;
	mutex_init(&data->mutex);

	i2c_set_clientdata(client, indio_dev);

	ret = qmc5883p_rf_init(data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize regmap fields\n");

	ret = qmc5883p_verify_chip_id(data);
	if (ret)
		return ret;

	ret = qmc5883p_chip_init(data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize chip\n");

	indio_dev->name = "qmc5883p";
	indio_dev->info = &qmc5883p_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = qmc5883p_channels;
	indio_dev->num_channels = ARRAY_SIZE(qmc5883p_channels);

	pm_runtime_set_autosuspend_delay(dev, 2000);
	pm_runtime_use_autosuspend(dev);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = devm_add_action_or_reset(dev,
				       (void (*)(void *))qmc5883p_runtime_pm_disable,
				       dev);
	if (ret)
		return ret;

	pm_runtime_mark_last_busy(dev);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IIO device\n");
	return 0;
}

static int qmc5883p_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct qmc5883p_data *data = iio_priv(indio_dev);

	return regmap_field_write(data->rf.mode, QMC5883P_MODE_SUSPEND);
}

static int qmc5883p_runtime_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct qmc5883p_data *data = iio_priv(indio_dev);
	int ret;

	ret = regmap_field_write(data->rf.mode, QMC5883P_MODE_NORMAL);
	if (ret)
		return ret;

	usleep_range(10000, 11000);
	return 0;
}

static int qmc5883p_runtime_idle(struct device *dev)
{
	return 0;
}

static int qmc5883p_system_suspend(struct device *dev)
{
	return pm_runtime_force_suspend(dev);
}

static int qmc5883p_system_resume(struct device *dev)
{
	return pm_runtime_force_resume(dev);
}

static void qmc5883p_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct qmc5883p_data *data = iio_priv(indio_dev);

	/*
	 * Best effort: put device to sleep on removal.
	 * Ignore error since we cannot do anything useful with it here.
	 * Runtime PM disable is handled by the devm cleanup action registered
	 * in probe.
	 */
	regmap_field_write(data->rf.mode, QMC5883P_MODE_SUSPEND);
}

static const struct dev_pm_ops qmc5883p_dev_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(qmc5883p_system_suspend, qmc5883p_system_resume)
	RUNTIME_PM_OPS(qmc5883p_runtime_suspend, qmc5883p_runtime_resume, NULL)
};

static const struct of_device_id qmc5883p_of_match[] = {
	{ .compatible = "qst,qmc5883p" },
	{}
};
MODULE_DEVICE_TABLE(of, qmc5883p_of_match);

static const struct i2c_device_id qmc5883p_id[] = {
	{ "qmc5883p", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, qmc5883p_id);

static struct i2c_driver qmc5883p_driver = {
	.driver = {
		.name = "qmc5883p",
		.of_match_table = qmc5883p_of_match,
		.pm = pm_ptr(&qmc5883p_dev_pm_ops),
	},
	.probe = qmc5883p_probe,
	.remove = qmc5883p_remove,
	.id_table = qmc5883p_id,
};
module_i2c_driver(qmc5883p_driver);

MODULE_AUTHOR("Hardik Phalet <hardik.phalet@pm.me>");
MODULE_DESCRIPTION("QMC5883P magnetic sensor driver");
MODULE_LICENSE("GPL");
