// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Invensense, Inc.
 * Copyright (C) 2026 Axis Communications AB
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <linux/iio/common/inv_sensors_timestamp.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include "inv_icm42370.h"

const struct regmap_config inv_icm42370_regmap_config = {
	.name = "inv_icm42370",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x7E,
};
EXPORT_SYMBOL_NS_GPL(inv_icm42370_regmap_config, "IIO_ICM42370");

static const struct inv_icm42370_conf inv_icm42370_default_conf = {
	.mode = INV_ICM42370_SENSOR_MODE_LOW_NOISE,
	.fs = INV_ICM42370_ACCEL_FS_16G,
	.odr = INV_ICM42370_ODR_400HZ,
	.filter = INV_ICM42370_FILTER_AVG_16X,
};

static const struct iio_chan_spec inv_icm42370_accel_channels[] = {
	INV_ICM42370_ACCEL_CHAN(IIO_MOD_X, INV_ICM42370_ACCEL_SCAN_X),
	INV_ICM42370_ACCEL_CHAN(IIO_MOD_Y, INV_ICM42370_ACCEL_SCAN_Y),
	INV_ICM42370_ACCEL_CHAN(IIO_MOD_Z, INV_ICM42370_ACCEL_SCAN_Z),
	INV_ICM42370_TEMP_CHAN(INV_ICM42370_ACCEL_SCAN_TEMP),
};

/* IIO format int + nano */
static const int inv_icm42370_accel_scale[] = {
	/* +/- 16G => 2*16*9.80665 / (2**15) m/s-2 */
	[2 * INV_ICM42370_ACCEL_FS_16G] = 0,
	[2 * INV_ICM42370_ACCEL_FS_16G + 1] = 9576807,
	/* +/- 8G => 2*8*9.80665 / (2**15) m/s-2 */
	[2 * INV_ICM42370_ACCEL_FS_8G] = 0,
	[2 * INV_ICM42370_ACCEL_FS_8G + 1] = 4788403,
	/* +/- 4G => 2*4*9.80665 / (2**15) m/s-2 */
	[2 * INV_ICM42370_ACCEL_FS_4G] = 0,
	[2 * INV_ICM42370_ACCEL_FS_4G + 1] = 2394202,
	/* +/- 2G => 2*2*9.80665 / (2**15) m/s-2 */
	[2 * INV_ICM42370_ACCEL_FS_2G] = 0,
	[2 * INV_ICM42370_ACCEL_FS_2G + 1] = 1197101,
};

/**
 *  inv_icm42370_odr_to_period() - map ODR to Period
 *  @odr - enum of ODR value
 *
 * Returns the period in nanoseconds
 */
u32 inv_icm42370_odr_to_period(enum inv_icm42370_odr odr)
{
	static u32 odr_periods[INV_ICM42370_ODR_NB] = {
		/* reserved values */
		0,
		0,
		0,
		0,
		0,
		/* 1.6kHz */
		625000,
		/* 800Hz */
		1250000,
		/* 400Hz */
		2500000,
		/* 200Hz */
		5000000,
		/* 100Hz */
		10000000,
		/* 50Hz */
		20000000,
		/* 25Hz */
		40000000,
		/* 12.5Hz */
		80000000,
		/* 6.25Hz */
		160000000,
		/* 3.125Hz */
		320000000,
		/* 1.5625Hz */
		640000000,
	};

	return odr_periods[odr];
}

/* ODR suffixed by LN or LP are Low-Noise or Low-Power mode only */
static const int inv_icm42370_accel_odr_conv[] = {
	INV_ICM42370_ODR_1_6KHZ_LN,   INV_ICM42370_ODR_800HZ_LN,
	INV_ICM42370_ODR_400HZ,	      INV_ICM42370_ODR_200HZ,
	INV_ICM42370_ODR_100HZ,	      INV_ICM42370_ODR_50HZ,
	INV_ICM42370_ODR_25HZ,	      INV_ICM42370_ODR_12_5HZ,
	INV_ICM42370_ODR_6_25HZ_LP,   INV_ICM42370_ODR_3_125HZ_LP,
	INV_ICM42370_ODR_1_5625HZ_LP, INV_ICM42370_ODR_NB,
};

/**
 * inv_icm42370_mreg_check() - check registers before accessing the registers in other banks
 *
 * @map: regmap of the device
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_mreg_check(struct regmap *map)
{
	int tmp;

	regmap_read(map, INV_ICM42370_REG_MCLK_RDY, &tmp);
	if (!(tmp & INV_ICM42370_MCLK_RDY_BIT))
		return -EINVAL;
	regmap_read(map, INV_ICM42370_REG_PWR_MGMT0, &tmp);
	if (tmp != INV_ICM42370_PWR_MGMT0(INV_ICM42370_SENSOR_MODE_LOW_NOISE))
		return -EINVAL;

	return 0;
}

/**
 * inv_icm42370_mreg_write() - routine for writing to other bank registers
 *
 * @map: regmap of the device
 * @bank: register bank being accessed
 * @addr: address of the register being accessed
 * @val: value written to the register
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_mreg_write(struct regmap *map, u8 bank, u8 addr, u8 val)
{
	int ret;

	ret = inv_icm42370_mreg_check(map);
	if (ret)
		return ret;

	ret = regmap_write(map, INV_ICM42370_REG_BLK_SEL_W, bank);
	if (ret)
		return -EINVAL;

	ret = regmap_write(map, INV_ICM42370_REG_MADDR_W, addr);
	if (ret)
		return -EINVAL;

	ret = regmap_write(map, INV_ICM42370_REG_M_W, val);
	if (ret)
		return -EINVAL;

	usleep_range(10, 20);
	return regmap_write(map, INV_ICM42370_REG_BLK_SEL_W, 0x00);
}

/**
 * inv_icm42370_mreg_read() - routine for reading from other bank registers
 *
 * @map: regmap of the device
 * @bank: register bank being accessed
 * @addr: address of the register being accessed
 * @val: pointer to store the register's data
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_mreg_read(struct regmap *map, u8 bank, u8 addr, u8 *val)
{
	int ret;
	unsigned int read_val;

	ret = inv_icm42370_mreg_check(map);
	if (ret)
		return ret;

	ret = regmap_write(map, INV_ICM42370_REG_BLK_SEL_R, bank);
	if (ret)
		return -EINVAL;

	ret = regmap_write(map, INV_ICM42370_REG_MADDR_R, addr);
	if (ret)
		return -EINVAL;

	usleep_range(10, 20);
	ret = regmap_read(map, INV_ICM42370_REG_M_R, &read_val);
	if (ret)
		return -EINVAL;

	usleep_range(10, 20);
	*val = (u8)read_val;

	return regmap_write(map, INV_ICM42370_REG_BLK_SEL_R, 0x00);
}

/**
 * inv_icm42370_set_conf() - set sensor configuration
 *
 * @data: pointer to struct containing the sensor data
 * @conf: pointer to configuration data
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_set_conf(struct inv_icm42370_data *data,
				 const struct inv_icm42370_conf *conf)
{
	unsigned int val;
	int ret;

	/* set PWR_MGMT0 register (accel sensor mode, temp enabled) */
	val = INV_ICM42370_PWR_MGMT0(conf->mode);
	ret = regmap_write(data->map, INV_ICM42370_REG_PWR_MGMT0, val);
	if (ret)
		return ret;

	msleep(200);
	/* set ACCEL_CONFIG0 register (accel fullscale & odr) */
	val = INV_ICM42370_ACCEL_CONFIG0_FS(conf->fs) |
	      INV_ICM42370_ACCEL_CONFIG0_ODR(conf->odr);
	ret = regmap_write(data->map, INV_ICM42370_REG_ACCEL_CONFIG0, val);
	if (ret)
		return ret;

	msleep(200);

	/* update internal conf */
	data->conf = *conf;

	return 0;
}

/**
 * inv_icm42370_set_pwr_mgmt0() - set the PWR_MGMT0 register for sensor
 *
 * @dev_data: pointer to struct containing the sensor data
 * @accel: enum of sensor power mode
 * @sleep_ms: pointer to check how long the sensor is in sleep mode
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_set_pwr_mgmt0(struct inv_icm42370_data *dev_data,
				      enum inv_icm42370_sensor_mode accel,
				      unsigned int *sleep_ms)
{
	enum inv_icm42370_sensor_mode oldaccel = dev_data->conf.mode;
	unsigned int sleepval;
	unsigned int val;
	int ret;

	/* if nothing changed, exit */
	if (accel == oldaccel)
		return 0;

	val = INV_ICM42370_PWR_MGMT0(accel);
	ret = regmap_write(dev_data->map, INV_ICM42370_REG_PWR_MGMT0, val);
	if (ret)
		return ret;

	dev_data->conf.mode = accel;
	dev_data->sensor_state->power_mode = accel;

	/* compute required wait time for sensors to stabilize */
	sleepval = 0;
	/* accel startup time */
	if (accel != oldaccel && oldaccel == INV_ICM42370_SENSOR_MODE_OFF) {
		/* block any register write for at least 200 µs */
		usleep_range(200, 300);
		if (sleepval < INV_ICM42370_ACCEL_STARTUP_TIME_MS)
			sleepval = INV_ICM42370_ACCEL_STARTUP_TIME_MS;
	}

	/* deferred sleep value if sleep pointer is provided or direct sleep */
	if (sleep_ms)
		*sleep_ms = sleepval;
	else if (sleepval)
		msleep(sleepval);

	return 0;
}

static void inv_icm42370_disable_pm(void *_data)
{
	struct device *dev = _data;

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
}

/**
 * inv_icm42370_set_accel_conf() - set configuration data for accelerometer
 *
 * @dev_data: pointer to struct containing the sensor data
 * @conf: pointer to configuration data
 * @sleep_ms: pointer to check how long the sensor is in sleep mode
 *
 * Returns 0 on success, negative errno on error
 */
int inv_icm42370_set_accel_conf(struct inv_icm42370_data *dev_data,
				struct inv_icm42370_conf *conf,
				unsigned int *sleep_ms)
{
	struct inv_icm42370_conf *oldconf = &dev_data->conf;
	unsigned int val;
	int ret;

	/* sanitize missing values with current values */
	if (conf->mode < 0)
		conf->mode = oldconf->mode;
	if (conf->fs < 0)
		conf->fs = oldconf->fs;
	if (conf->odr < 0)
		conf->odr = oldconf->odr;
	if (conf->filter < 0)
		conf->filter = oldconf->filter;

	/* force power mode against ODR when sensor is on */
	switch (conf->mode) {
	case INV_ICM42370_SENSOR_MODE_LOW_POWER:
	case INV_ICM42370_SENSOR_MODE_LOW_NOISE:
		if (conf->odr <= INV_ICM42370_ODR_800HZ_LN) {
			conf->mode = INV_ICM42370_SENSOR_MODE_LOW_NOISE;
			conf->filter =
				INV_ICM42370_UI_FILT_BW_LP_FILTER_BYPASSED;
		} else if (conf->odr == INV_ICM42370_ODR_400HZ) {
			if (conf->filter == INV_ICM42370_FILTER_AVG_16X ||
			    conf->filter == INV_ICM42370_FILTER_AVG_32X ||
			    conf->filter == INV_ICM42370_FILTER_AVG_64X) {
				conf->mode = INV_ICM42370_SENSOR_MODE_LOW_NOISE;
			} else {
				conf->mode = INV_ICM42370_SENSOR_MODE_LOW_POWER;
			}
		} else if (conf->odr == INV_ICM42370_ODR_200HZ &&
			   conf->filter == INV_ICM42370_FILTER_AVG_64X) {
			conf->mode = INV_ICM42370_SENSOR_MODE_LOW_NOISE;
			conf->filter =
				INV_ICM42370_UI_FILT_BW_LP_FILTER_BYPASSED;
		} else if (conf->odr >= INV_ICM42370_ODR_6_25HZ_LP) {
			conf->mode = INV_ICM42370_SENSOR_MODE_LOW_POWER;
			conf->filter = INV_ICM42370_FILTER_AVG_16X;
		}
		break;
	default:
		break;
	}

	/* set ACCEL_CONFIG0 register (accel fullscale & odr) */
	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = INV_ICM42370_ACCEL_CONFIG0_FS(conf->fs) |
		      INV_ICM42370_ACCEL_CONFIG0_ODR(conf->odr);
		ret = regmap_write(dev_data->map,
				   INV_ICM42370_REG_ACCEL_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	/* set PWR_MGMT0 register (accel sensor mode) */
	return inv_icm42370_set_pwr_mgmt0(dev_data, dev_data->conf.mode,
					  sleep_ms);
}

/**
 * inv_icm42370_setup() - check and setup chip
 *
 * @data: pointer to struct containing the sensor data
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm42370_setup(struct inv_icm42370_data *data,
			      inv_icm42370_bus_setup bus_setup)
{
	const struct device *dev = regmap_get_device(data->map);
	unsigned int whoami;
	int ret;

	/* check chip self-identification value */
	ret = regmap_read(data->map, INV_ICM42370_REG_WHO_AM_I, &whoami);
	if (ret)
		return ret;

	if (whoami != INV_ICM42370_WHOAMI_VALUE) {
		dev_err(dev, "Wrong WHO_AM_I: %d (want 0x%02X)\n",
				     whoami, INV_ICM42370_WHOAMI_VALUE);
		return -ENODEV;
	}

	data->name = "inv_icm42370";

	/* set chip bus configuration */
	ret = bus_setup(data);
	if (ret)
		return ret;

	/* sensor data in big-endian (default) */
	ret = regmap_set_bits(data->map, INV_ICM42370_REG_INTF_CONFIG0,
			      INV_ICM42370_INTF_CONFIG0_SENSOR_DATA_ENDIAN);
	if (ret)
		return ret;

	return inv_icm42370_set_conf(data, &inv_icm42370_default_conf);
}

static irqreturn_t inv_icm42370_irq_timestamp(int irq, void *_data)
{
	struct inv_icm42370_data *dev_data = _data;

	dev_data->timestamp = iio_get_time_ns(dev_data->indio_accel);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t inv_icm42370_irq_handler(int irq, void *_data)
{
	struct inv_icm42370_data *dev_data = _data;
	unsigned int status;
	int ret;

	mutex_lock(&dev_data->lock);

	ret = regmap_read(dev_data->map, INV_ICM42370_REG_INT_STATUS, &status);
	if (ret)
		goto out_unlock;

out_unlock:
	mutex_unlock(&dev_data->lock);
	return IRQ_HANDLED;
}

/**
 * inv_icm42370_irq_init() - initialize int pin and interrupt handler
 * @data:		driver internal state
 * @irq:	irq number
 * @irq_type:	irq trigger type
 * @open_drain:	true if irq is open drain, false for push-pull
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm42370_irq_init(struct inv_icm42370_data *data, int irq,
				 int irq_type, bool open_drain)
{
	struct device *dev = regmap_get_device(data->map);
	u8 val;
	int ret;

	/* configure INT1 interrupt: default is active low on edge */
	switch (irq_type) {
	case IRQF_TRIGGER_RISING:
	case IRQF_TRIGGER_HIGH:
		val = INV_ICM42370_INT_CONFIG_INT1_ACTIVE_HIGH;
		break;
	default:
		val = INV_ICM42370_INT_CONFIG_INT1_ACTIVE_LOW;
		break;
	}

	switch (irq_type) {
	case IRQF_TRIGGER_LOW:
	case IRQF_TRIGGER_HIGH:
		val |= INV_ICM42370_INT_CONFIG_INT1_LATCHED;
		break;
	default:
		break;
	}

	if (!open_drain)
		val |= INV_ICM42370_INT_CONFIG_INT1_PUSH_PULL;

	ret = regmap_write(data->map, INV_ICM42370_REG_INT_CONFIG, val);
	if (ret)
		return ret;

	/* Deassert async reset for proper INT pin operation (cf datasheet) */
	ret = inv_icm42370_mreg_read(data->map, INV_ICM42370_MREG1,
				     INV_ICM42370_REG_INT_CONFIG1, &val);
	if (ret)
		return ret;
	val &= ~INV_ICM42370_INT_CONFIG1_ASYNC_RESET;

	ret = inv_icm42370_mreg_write(data->map, INV_ICM42370_MREG1,
				      INV_ICM42370_REG_INT_CONFIG1, val);
	if (ret)
		return ret;

	irq_type |= IRQF_ONESHOT;
	return devm_request_threaded_irq(dev, irq, inv_icm42370_irq_timestamp,
					 inv_icm42370_irq_handler, irq_type,
					 "inv_icm42370", data);
}

/*
 * Calibration bias values, IIO range format int + micro.
 * Value is limited to +/-1g coded on 12 bits signed. Step is 0.5mg.
 */
static int inv_icm42370_accel_calibbias[] = {
	-10, 42010, /* min: -2^12 * 0.0005 * 9.80665 = -10.042010 m/s² */
	0,   4903, /* step: 0.5 * 0.00980655 = 0.004903 m/s² */
	10,  37106, /* max: (2^12 - 1) * 0.0005 * 9.80665 = 10.037106 m/s² */
};

/**
 * inv_icm42370_temp_read() - internal function to access temperature sensor registers
 *
 * @dev_data: pointer to struct containing the sensor data
 * @temp: pointer containing the temperature data in s16 format
 *
 * Return 0 on success, negative errno on error
 */
static int inv_icm42370_temp_read(struct inv_icm42370_data *dev_data, s16 *temp)
{
	struct device *dev = regmap_get_device(dev_data->map);
	__be16 *raw;
	int ret;

	pm_runtime_get_sync(dev);
	mutex_lock(&dev_data->lock);

	raw = (__be16 *)&dev_data->buffer[0];
	ret = regmap_bulk_read(dev_data->map, INV_ICM42370_REG_TEMP_DATA1, raw,
			       sizeof(*raw));
	if (ret)
		goto exit;

	*temp = (s16)be16_to_cpup(raw);

	/*
	 * Temperature data is invalid if both accel and gyro are off.
	 * Return -EBUSY in this case.
	 */
	if (*temp == INV_ICM42370_DATA_INVALID)
		ret = -EBUSY;

exit:
	mutex_unlock(&dev_data->lock);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

/**
 * inv_icm42370_temp_read_raw() - read data from the temperature sensor
 *
 * @indio_dev: pointer to the industrial io struct
 * @chan: pointer to the iio channel specification
 * @val: integer part of the value
 * @val2: decimal part of the value
 * @mask: mask to differentiate between channel info
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_temp_read_raw(struct iio_dev *indio_dev,
				      struct iio_chan_spec const *chan,
				      int *val, int *val2, long mask)
{
	struct inv_icm42370_data *dev_data = iio_priv(indio_dev);
	s16 temp;
	int ret;

	if (chan->type != IIO_TEMP)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm42370_temp_read(dev_data, &temp);
		iio_device_release_direct(indio_dev);
		if (ret)
			return ret;
		*val = temp;
		return IIO_VAL_INT;
	/*
	 * T°C = (temp / 128) + 25
	 * Tm°C = 1000 * ((temp / 128) + 25)
	 * Tm°C = 7.8125 * temp + 25000
	 * Tm°C = (temp + 3200) * 7.8125
	 * scale: 1000 / 128 ~= 7.8125
	 * offset: 3200
	 */
	case IIO_CHAN_INFO_SCALE:
		*val = 7;
		*val2 = 812500;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_OFFSET:
		*val = 3200;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

/**
 * inv_icm42370_accel_read_offset() - read offset values from the accelerometer
 *
 * @dev_data: pointer to struct containing the sensor data
 * @chan: pointer to iio channel specification
 * @val: integer part of the value
 * @val2: decimal part of the value
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_accel_read_offset(struct inv_icm42370_data *dev_data,
					  struct iio_chan_spec const *chan,
					  int *val, int *val2)
{
	struct device *dev = regmap_get_device(dev_data->map);
	s64 val64;
	s32 bias;
	unsigned int reg;
	s16 offset;
	u8 data[2];
	int ret;

	if (chan->type != IIO_ACCEL)
		return -EINVAL;

	switch (chan->channel2) {
	case IIO_MOD_X:
		reg = INV_ICM42370_REG_OFFSET_USER4;
		break;
	case IIO_MOD_Y:
		reg = INV_ICM42370_REG_OFFSET_USER6;
		break;
	case IIO_MOD_Z:
		reg = INV_ICM42370_REG_OFFSET_USER7;
		break;
	default:
		return -EINVAL;
	}

	pm_runtime_get_sync(dev);
	mutex_lock(&dev_data->lock);

	ret = inv_icm42370_mreg_read(dev_data->map, INV_ICM42370_MREG1, reg,
				     dev_data->buffer);
	memcpy(data, dev_data->buffer, sizeof(data));

	mutex_unlock(&dev_data->lock);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	if (ret)
		return ret;

	/* 12 bits signed value */
	switch (chan->channel2) {
	case IIO_MOD_X:
		offset = sign_extend32(((data[0] & 0xF0) << 4) | data[1], 11);
		break;
	case IIO_MOD_Y:
		offset = sign_extend32(((data[1] & 0x0F) << 8) | data[0], 11);
		break;
	case IIO_MOD_Z:
		offset = sign_extend32(((data[0] & 0xF0) << 4) | data[1], 11);
		break;
	default:
		return -EINVAL;
	}

	/*
	 * convert raw offset to g then to m/s²
	 * 12 bits signed raw step 0.5mg to g: 5 / 10000
	 * g to m/s²: 9.806650
	 * result in micro (1000000)
	 * (offset * 5 * 9.806650 * 1000000) / 10000
	 */
	val64 = (s64)offset * 5LL * 9806650LL;
	/* for rounding, add + or - divisor (10000) divided by 2 */
	if (val64 >= 0)
		val64 += 10000LL / 2LL;
	else
		val64 -= 10000LL / 2LL;
	bias = div_s64(val64, 10000L);
	*val = bias / 1000000L;
	*val2 = bias % 1000000L;

	return IIO_VAL_INT_PLUS_MICRO;
}

/**
 * inv_icm42370_accel_write_offset() - write offset values to the accelerometer
 *
 * @dev_data: pointer to struct containing the sensor data
 * @chan: pointer to iio channel specification
 * @val: integer part of the value
 * @val2: decimal part of the value
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_accel_write_offset(struct inv_icm42370_data *dev_data,
					   struct iio_chan_spec const *chan,
					   int val, int val2)
{
	struct device *dev = regmap_get_device(dev_data->map);
	s64 val64;
	s32 min, max;
	unsigned int regval;
	s16 offset;
	int ret;

	if (chan->type != IIO_ACCEL)
		return -EINVAL;

	/* inv_icm42370_accel_calibbias: min - step - max in micro */
	min = inv_icm42370_accel_calibbias[0] * 1000000L +
	      inv_icm42370_accel_calibbias[1];
	max = inv_icm42370_accel_calibbias[4] * 1000000L +
	      inv_icm42370_accel_calibbias[5];
	val64 = (s64)val * 1000000LL + (s64)val2;
	if (val64 < min || val64 > max)
		return -EINVAL;

	/*
	 * convert m/s² to g then to raw value
	 * m/s² to g: 1 / 9.806650
	 * g to raw 12 bits signed, step 0.5mg: 10000 / 5
	 * val in micro (1000000)
	 * val * 10000 / (9.806650 * 1000000 * 5)
	 */
	val64 = val64 * 10000LL;
	/* for rounding, add + or - divisor (9806650 * 5) divided by 2 */
	if (val64 >= 0)
		val64 += 9806650 * 5 / 2;
	else
		val64 -= 9806650 * 5 / 2;
	offset = div_s64(val64, 9806650 * 5);

	/* clamp value limited to 12 bits signed */
	if (offset < -2048)
		offset = -2048;
	else if (offset > 2047)
		offset = 2047;

	pm_runtime_get_sync(dev);
	mutex_lock(&dev_data->lock);

	switch (chan->channel2) {
	case IIO_MOD_X:
		/* OFFSET_USER4 register is shared */
		ret = inv_icm42370_mreg_read(dev_data->map, INV_ICM42370_MREG1,
					     INV_ICM42370_REG_OFFSET_USER4,
					     (u8 *)&regval);
		if (ret)
			goto out_unlock;
		dev_data->buffer[0] = ((offset & 0xF00) >> 4) | (regval & 0x0F);
		dev_data->buffer[1] = offset & 0xFF;

		ret = inv_icm42370_mreg_write(dev_data->map, INV_ICM42370_MREG1,
					      INV_ICM42370_REG_OFFSET_USER4,
					      dev_data->buffer[0]);
		if (ret)
			goto out_unlock;

		ret = inv_icm42370_mreg_write(dev_data->map, INV_ICM42370_MREG1,
					      INV_ICM42370_REG_OFFSET_USER5,
					      dev_data->buffer[1]);
		if (ret)
			goto out_unlock;
		break;
	case IIO_MOD_Y:
		/* OFFSET_USER7 register is shared */
		ret = inv_icm42370_mreg_read(dev_data->map, INV_ICM42370_MREG1,
					     INV_ICM42370_REG_OFFSET_USER7,
					     (u8 *)&regval);
		if (ret)
			goto out_unlock;
		dev_data->buffer[0] = offset & 0xFF;
		dev_data->buffer[1] = ((offset & 0xF00) >> 8) | (regval & 0xF0);

		ret = inv_icm42370_mreg_write(dev_data->map, INV_ICM42370_MREG1,
					      INV_ICM42370_REG_OFFSET_USER7,
					      dev_data->buffer[0]);
		if (ret)
			goto out_unlock;

		ret = inv_icm42370_mreg_write(dev_data->map, INV_ICM42370_MREG1,
					      INV_ICM42370_REG_OFFSET_USER6,
					      dev_data->buffer[1]);
		if (ret)
			goto out_unlock;

		break;
	case IIO_MOD_Z:
		/* OFFSET_USER7 register is shared */
		inv_icm42370_mreg_read(dev_data->map, INV_ICM42370_MREG1,
				       INV_ICM42370_REG_OFFSET_USER7,
				       (u8 *)&regval);

		dev_data->buffer[0] = ((offset & 0xF00) >> 4) | (regval & 0x0F);
		dev_data->buffer[1] = offset & 0xFF;

		ret = inv_icm42370_mreg_write(dev_data->map, INV_ICM42370_MREG1,
					      INV_ICM42370_REG_OFFSET_USER7,
					      dev_data->buffer[0]);
		if (ret)
			goto out_unlock;

		ret = inv_icm42370_mreg_write(dev_data->map, INV_ICM42370_MREG1,
					      INV_ICM42370_REG_OFFSET_USER8,
					      dev_data->buffer[1]);
		if (ret)
			goto out_unlock;
		break;
	default:
		ret = -EINVAL;
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&dev_data->lock);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

/**
 * inv_icm42370_accel_read_scale() - read scaling data from the accelerometer
 *
 * @indio_dev: pointer to the industrial io struct
 * @val: integer part of the value
 * @val2: decimal part of the value
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_accel_read_scale(struct iio_dev *indio_dev, int *val,
					 int *val2)
{
	struct inv_icm42370_data *dev_data = iio_priv(indio_dev);
	unsigned int idx;

	idx = dev_data->conf.fs;

	*val = dev_data->sensor_state->scales[2 * idx];
	*val2 = dev_data->sensor_state->scales[2 * idx + 1];
	return IIO_VAL_INT_PLUS_NANO;
}

/**
 * inv_icm42370_accel_write_scale() - write scaling data to the accelerometer
 *
 * @indio_dev: pointer to the industrial io struct
 * @val: integer part of the value
 * @val2: decimal part of the value
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_accel_write_scale(struct iio_dev *indio_dev, int val,
					  int val2)
{
	struct inv_icm42370_data *dev_data = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(dev_data->map);
	unsigned int idx;
	struct inv_icm42370_conf conf = INV_ICM42370_SENSOR_CONF_INIT;
	int ret;

	for (idx = 0; idx < dev_data->sensor_state->scales_len; idx += 2) {
		if (val == dev_data->sensor_state->scales[idx] &&
		    val2 == dev_data->sensor_state->scales[idx + 1])
			break;
	}

	if (idx >= dev_data->sensor_state->scales_len)
		return -EINVAL;

	conf.fs = idx / 2;

	pm_runtime_get_sync(dev);
	mutex_lock(&dev_data->lock);

	ret = inv_icm42370_set_accel_conf(dev_data, &conf, NULL);

	mutex_unlock(&dev_data->lock);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

/**
 * inv_icm42370_accel_read_odr() - read ODR data from the accelerometer
 *
 * @dev_data: pointer to struct containing the sensor data
 * @val: integer part of the value
 * @val2: decimal part of the value
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_accel_read_odr(struct inv_icm42370_data *dev_data,
				       int *val, int *val2)
{
	unsigned int odr;
	unsigned int i;

	odr = dev_data->conf.odr;

	for (i = 0; i < ARRAY_SIZE(inv_icm42370_accel_odr_conv); ++i) {
		if (inv_icm42370_accel_odr_conv[i] == odr)
			break;
	}
	if (i >= ARRAY_SIZE(inv_icm42370_accel_odr_conv))
		return -EINVAL;

	*val = inv_icm42370_accel_odr[2 * i];
	*val2 = inv_icm42370_accel_odr[2 * i + 1];

	return IIO_VAL_INT_PLUS_MICRO;
}

/**
 * inv_icm42370_accel_write_odr() - write ODR data to the accelerometer
 *
 * @indio_dev: pointer to struct containing the sensor data
 * @val: integer part of the value
 * @val2: decimal part of the value
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_accel_write_odr(struct iio_dev *indio_dev, int val,
					int val2)
{
	struct inv_icm42370_data *dev_data = iio_priv(indio_dev);
	struct inv_sensors_timestamp *ts = &dev_data->sensor_state->ts;
	struct device *dev = regmap_get_device(dev_data->map);
	unsigned int idx;
	struct inv_icm42370_conf conf = INV_ICM42370_SENSOR_CONF_INIT;
	int ret;

	for (idx = 0; idx < ARRAY_SIZE(inv_icm42370_accel_odr); idx += 2) {
		if (val == inv_icm42370_accel_odr[idx] &&
		    val2 == inv_icm42370_accel_odr[idx + 1])
			break;
	}
	if (idx >= ARRAY_SIZE(inv_icm42370_accel_odr))
		return -EINVAL;

	conf.odr = inv_icm42370_accel_odr_conv[idx / 2];

	pm_runtime_get_sync(dev);
	mutex_lock(&dev_data->lock);

	ret = inv_sensors_timestamp_update_odr(
		ts, inv_icm42370_odr_to_period(conf.odr),
		iio_buffer_enabled(indio_dev));
	if (ret)
		goto out_unlock;

	ret = inv_icm42370_set_accel_conf(dev_data, &conf, NULL);
	if (ret)
		goto out_unlock;

out_unlock:
	mutex_unlock(&dev_data->lock);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

/**
 * inv_icm42370_accel_write_raw() - write raw attribute values to the accelerometer
 * @indio_dev:	pointer to the IIO device structure.
 * @chan:	pointer to the IIO channel specification.
 * @val:	integer part of the value to write.
 * @val2:	fractional part of the value to write.
 * @mask:	bitmask specifying which attribute to write.
 *
 * Returns 0 on success, negative errno on error.
 */
static int inv_icm42370_accel_write_raw(struct iio_dev *indio_dev,
					struct iio_chan_spec const *chan,
					int val, int val2, long mask)
{
	struct inv_icm42370_data *dev_data = iio_priv(indio_dev);
	int ret;

	if (chan->type != IIO_ACCEL)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm42370_accel_write_scale(indio_dev, val, val2);
		iio_device_release_direct(indio_dev);
		return ret;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return inv_icm42370_accel_write_odr(indio_dev, val, val2);
	case IIO_CHAN_INFO_CALIBBIAS:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm42370_accel_write_offset(dev_data, chan, val,
						      val2);
		iio_device_release_direct(indio_dev);
		return ret;

	default:
		return -EINVAL;
	}
}

/**
 * inv_icm42370_accel_read_sensor() - internal function to read accelerometer sensor registers
 *
 * @indio_dev: pointer to the industrial io struct
 * @chan: pointer to iio channel specification
 * @val: pointer containing accelerometer data in s16 format
 *
 * Return 0 on success, negative errno on error
 */
static int inv_icm42370_accel_read_sensor(struct iio_dev *indio_dev,
					  struct iio_chan_spec const *chan,
					  s16 *val)
{
	struct inv_icm42370_data *data = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(data->map);
	unsigned int reg;
	__be16 *value;
	int ret;

	if (chan->type != IIO_ACCEL)
		return -EINVAL;

	switch (chan->channel2) {
	case IIO_MOD_X:
		reg = INV_ICM42370_REG_ACCEL_DATA_X1;
		break;
	case IIO_MOD_Y:
		reg = INV_ICM42370_REG_ACCEL_DATA_Y1;
		break;
	case IIO_MOD_Z:
		reg = INV_ICM42370_REG_ACCEL_DATA_Z1;
		break;
	default:
		return -EINVAL;
	}

	pm_runtime_get_sync(dev);
	mutex_lock(&data->lock);

	/* read accel register data */
	value = (__be16 *)&data->buffer[0];
	ret = regmap_bulk_read(data->map, reg, value, sizeof(*value));
	if (ret)
		goto exit;

	*val = (s16)be16_to_cpup(value);

	if (*val == INV_ICM42370_DATA_INVALID)
		ret = -EINVAL;

exit:
	mutex_unlock(&data->lock);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

/**
 * inv_icm42370_accel_read_raw() - read data from the accelerometer sensor
 *
 * @indio_dev: pointer to the industrial io struct
 * @chan: pointer to the iio channel specification
 * @val: integer part of the value
 * @val2: decimal part of the value
 * @mask: mask to differentiate between channel info
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_accel_read_raw(struct iio_dev *indio_dev,
				       struct iio_chan_spec const *chan,
				       int *val, int *val2, long mask)
{
	struct inv_icm42370_data *dev_data = iio_priv(indio_dev);
	s16 value;
	int ret;

	switch (chan->type) {
	case IIO_ACCEL:
		break;
	case IIO_TEMP:
		return inv_icm42370_temp_read_raw(indio_dev, chan, val, val2,
						  mask);
	default:
		return -EINVAL;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm42370_accel_read_sensor(indio_dev, chan, &value);
		iio_device_release_direct(indio_dev);
		if (ret)
			return ret;
		*val = value;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		return inv_icm42370_accel_read_scale(indio_dev, val, val2);
	case IIO_CHAN_INFO_SAMP_FREQ:
		return inv_icm42370_accel_read_odr(dev_data, val, val2);
	case IIO_CHAN_INFO_CALIBBIAS:
		return inv_icm42370_accel_read_offset(dev_data, chan, val,
						      val2);
	default:
		return -EINVAL;
	}
}

static const struct iio_info inv_icm42370_info = {
	.read_raw = inv_icm42370_accel_read_raw,
	.write_raw = inv_icm42370_accel_write_raw,
};

struct iio_dev *inv_icm42370_accel_init(struct iio_dev *indio_dev,
					struct inv_icm42370_data *data)
{
	struct device *dev = regmap_get_device(data->map);
	struct inv_sensors_timestamp_chip ts_chip;
	int ret;

	data->sensor_state->scales = inv_icm42370_accel_scale;
	data->sensor_state->scales_len = ARRAY_SIZE(inv_icm42370_accel_scale);

	/*
	 * clock period is 32kHz (31250ns)
	 * jitter is +/- 2% (20 per mille)
	 */
	ts_chip.clock_period = 31250;
	ts_chip.jitter = 20;
	ts_chip.init_period = inv_icm42370_odr_to_period(data->conf.odr);
	inv_sensors_timestamp_init(&data->sensor_state->ts, &ts_chip);

	indio_dev->name = "inv_icm42370";
	indio_dev->info = &inv_icm42370_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = inv_icm42370_accel_channels;
	indio_dev->num_channels = ARRAY_SIZE(inv_icm42370_accel_channels);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return ERR_PTR(ret);

	return indio_dev;
}

/**
 * inv_icm42370_core_probe() - initialize and register the ICM-42370 device
 * @regmap:	register map for accessing the device's registers.
 * @chip:	chip identifier, must be %INV_CHIP_ICM42370.
 * @irq:	interrupt number for the device's data-ready signal.
 * @bus_setup:	callback to configure bus-specific settings (e.g. I2C).
 *
 * Returns 0 on success, a negative error code otherwise.
 */
int inv_icm42370_core_probe(struct regmap *regmap, int chip, int irq,
			    inv_icm42370_bus_setup bus_setup)
{
	struct device *dev = regmap_get_device(regmap);
	struct inv_icm42370_data *data;
	struct iio_dev *indio_dev;
	struct irq_data *irq_desc;
	int irq_type;
	bool open_drain;
	int ret;

	if (chip != INV_CHIP_ICM42370) {
		dev_err(dev, "invalid chip = %d\n", chip);
		return -ENODEV;
	}

	/* get irq properties, set trigger falling by default */
	irq_desc = irq_get_irq_data(irq);
	if (!irq_desc) {
		dev_err(dev, "could not find IRQ %d\n", irq);
		return -EINVAL;
	}

	irq_type = irqd_get_trigger_type(irq_desc);
	if (!irq_type)
		irq_type = IRQF_TRIGGER_FALLING;

	open_drain = device_property_read_bool(dev, "drive-open-drain");

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->sensor_state =
		devm_kzalloc(dev, sizeof(*data->sensor_state), GFP_KERNEL);
	if (!data->sensor_state)
		return -ENOMEM;

	mutex_init(&data->lock);
	data->chip = chip;
	data->map = regmap;

	data->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(data->vdd_supply))
		return PTR_ERR(data->vdd_supply);

	data->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(data->vddio_supply))
		return PTR_ERR(data->vddio_supply);

	ret = regulator_enable(data->vdd_supply);
	if (ret)
		return ret;

	ret = inv_icm42370_setup(data, bus_setup);
	if (ret)
		return dev_err_probe(dev, ret, "Setup failed\n");

	data->indio_accel = inv_icm42370_accel_init(indio_dev, data);
	if (IS_ERR(data->indio_accel))
		return PTR_ERR(data->indio_accel);

	ret = inv_icm42370_irq_init(data, irq, irq_type, open_drain);
	if (ret)
		return ret;

	/* setup runtime power management */
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;

	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put(dev);

	return devm_add_action_or_reset(dev, inv_icm42370_disable_pm, dev);
}
EXPORT_SYMBOL_NS_GPL(inv_icm42370_core_probe, "IIO_ICM42370");

MODULE_AUTHOR("Kanak Shilledar <kanak.shilledar@axis.com>");
MODULE_AUTHOR("Henrik Grimler <henrik.grimler@axis.com>");
MODULE_DESCRIPTION("Invensense device ICM42370 driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_INV_SENSORS_TIMESTAMP");
