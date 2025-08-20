// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2025 Invensense, Inc. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>

#include "inv_icm45600_buffer.h"
#include "inv_icm45600.h"

static int inv_icm45600_ireg_read(struct regmap *map, unsigned int reg,
				   u8 *data, size_t count)
{
	const struct device *dev = regmap_get_device(map);
	struct inv_icm45600_state *st = dev_get_drvdata(dev);
	unsigned int d;
	ssize_t i;
	int ret;

	st->buffer.ireg[0] = FIELD_GET(INV_ICM45600_REG_BANK_MASK, reg);
	st->buffer.ireg[1] = FIELD_GET(INV_ICM45600_REG_ADDR_MASK, reg);

	/* Burst write address. */
	ret = regmap_bulk_write(map, INV_ICM45600_REG_IREG_ADDR, st->buffer.ireg, 2);
	/* Wait while the device is busy processing the address. */
	fsleep(INV_ICM45600_IREG_DELAY_US);
	if (ret)
		return ret;

	/* Read the data. */
	for (i = 0; i < count; i++) {
		ret = regmap_read(map, INV_ICM45600_REG_IREG_DATA, &d);
		/* Wait while the device is busy processing the data. */
		fsleep(INV_ICM45600_IREG_DELAY_US);
		if (ret)
			return ret;
		data[i] = d;
	}

	return 0;
}

static int inv_icm45600_ireg_write(struct regmap *map, unsigned int reg,
				   const u8 *data, size_t count)
{
	const struct device *dev = regmap_get_device(map);
	struct inv_icm45600_state *st = dev_get_drvdata(dev);
	ssize_t i;
	int ret;

	st->buffer.ireg[0] = FIELD_GET(INV_ICM45600_REG_BANK_MASK, reg);
	st->buffer.ireg[1] = FIELD_GET(INV_ICM45600_REG_ADDR_MASK, reg);
	st->buffer.ireg[2] = data[0];

	/* Burst write address and first byte. */
	ret = regmap_bulk_write(map, INV_ICM45600_REG_IREG_ADDR, st->buffer.ireg, 3);
	/* Wait while the device is busy processing the address and data. */
	fsleep(INV_ICM45600_IREG_DELAY_US);
	if (ret)
		return ret;

	/* Write the remaining bytes. */
	for (i = 1; i < count; i++) {
		ret = regmap_write(map, INV_ICM45600_REG_IREG_DATA, data[i]);
		/* Wait while the device is busy processing the data. */
		fsleep(INV_ICM45600_IREG_DELAY_US);
		if (ret)
			return ret;
	}

	return 0;
}

static int inv_icm45600_read(void *context, const void *reg_buf, size_t reg_size,
			  void *val_buf, size_t val_size)
{
	unsigned int reg = be16_to_cpup(reg_buf);
	struct regmap *map = context;

	if (FIELD_GET(INV_ICM45600_REG_BANK_MASK, reg) == 0)
		return regmap_bulk_read(map, FIELD_GET(INV_ICM45600_REG_ADDR_MASK, reg),
					val_buf, val_size);

	return inv_icm45600_ireg_read(map, reg, val_buf, val_size);
}

static int inv_icm45600_write(void *context, const void *data,
				   size_t count)
{
	const u8 *d = data;
	unsigned int reg = be16_to_cpup(data);
	struct regmap *map = context;

	if (FIELD_GET(INV_ICM45600_REG_BANK_MASK, reg) == 0)
		return regmap_bulk_write(map, FIELD_GET(INV_ICM45600_REG_ADDR_MASK, reg),
					 d + 2, count - 2);

	return inv_icm45600_ireg_write(map, reg, d + 2, count - 2);
}

static const struct regmap_bus inv_icm45600_regmap_bus = {
	.read = inv_icm45600_read,
	.write = inv_icm45600_write,
};

static const struct regmap_config inv_icm45600_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
};

/* These are the chip initial default configurations (default FS value is based on icm45686) */
static const struct inv_icm45600_conf inv_icm45600_default_conf = {
	.gyro = {
		.mode = INV_ICM45600_SENSOR_MODE_OFF,
		.fs = INV_ICM45686_GYRO_FS_2000DPS,
		.odr = INV_ICM45600_ODR_800HZ_LN,
		.filter = INV_ICM45600_GYRO_LP_AVG_SEL_8X,
	},
	.accel = {
		.mode = INV_ICM45600_SENSOR_MODE_OFF,
		.fs = INV_ICM45686_ACCEL_FS_16G,
		.odr = INV_ICM45600_ODR_800HZ_LN,
		.filter = INV_ICM45600_ACCEL_LP_AVG_SEL_4X,
	},
};

static const struct inv_icm45600_conf inv_icm45686_default_conf = {
	.gyro = {
		.mode = INV_ICM45600_SENSOR_MODE_OFF,
		.fs = INV_ICM45686_GYRO_FS_4000DPS,
		.odr = INV_ICM45600_ODR_800HZ_LN,
		.filter = INV_ICM45600_GYRO_LP_AVG_SEL_8X,
	},
	.accel = {
		.mode = INV_ICM45600_SENSOR_MODE_OFF,
		.fs = INV_ICM45686_ACCEL_FS_32G,
		.odr = INV_ICM45600_ODR_800HZ_LN,
		.filter = INV_ICM45600_ACCEL_LP_AVG_SEL_4X,
	},
};

const struct inv_icm45600_chip_info inv_icm45605_chip_info = {
	.whoami = INV_ICM45600_WHOAMI_ICM45605,
	.name = "icm45605",
	.conf = &inv_icm45600_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm45605_chip_info, "IIO_ICM45600");

const struct inv_icm45600_chip_info inv_icm45606_chip_info = {
	.whoami = INV_ICM45600_WHOAMI_ICM45606,
	.name = "icm45606",
	.conf = &inv_icm45600_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm45606_chip_info, "IIO_ICM45600");

const struct inv_icm45600_chip_info inv_icm45608_chip_info = {
	.whoami = INV_ICM45600_WHOAMI_ICM45608,
	.name = "icm45608",
	.conf = &inv_icm45600_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm45608_chip_info, "IIO_ICM45600");

const struct inv_icm45600_chip_info inv_icm45634_chip_info = {
	.whoami = INV_ICM45600_WHOAMI_ICM45634,
	.name = "icm45634",
	.conf = &inv_icm45600_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm45634_chip_info, "IIO_ICM45600");

const struct inv_icm45600_chip_info inv_icm45686_chip_info = {
	.whoami = INV_ICM45600_WHOAMI_ICM45686,
	.name = "icm45686",
	.conf = &inv_icm45686_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm45686_chip_info, "IIO_ICM45600");

const struct inv_icm45600_chip_info inv_icm45687_chip_info = {
	.whoami = INV_ICM45600_WHOAMI_ICM45687,
	.name = "icm45687",
	.conf = &inv_icm45686_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm45687_chip_info, "IIO_ICM45600");

const struct inv_icm45600_chip_info inv_icm45688p_chip_info = {
	.whoami = INV_ICM45600_WHOAMI_ICM45688P,
	.name = "icm45688p",
	.conf = &inv_icm45686_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm45688p_chip_info, "IIO_ICM45600");

const struct inv_icm45600_chip_info inv_icm45689_chip_info = {
	.whoami = INV_ICM45600_WHOAMI_ICM45689,
	.name = "icm45689",
	.conf = &inv_icm45686_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm45689_chip_info, "IIO_ICM45600");

const struct iio_mount_matrix *
inv_icm45600_get_mount_matrix(const struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan)
{
	const struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);

	return &st->orientation;
}

u32 inv_icm45600_odr_to_period(enum inv_icm45600_odr odr)
{
	static const u32 odr_periods[INV_ICM45600_ODR_MAX] = {
		0, 0, 0,	/* reserved values */
		156250,		/* 6.4kHz */
		312500,		/* 3.2kHz */
		625000,		/* 1.6kHz */
		1250000,	/* 800kHz */
		2500000,	/* 400Hz */
		5000000,	/* 200Hz */
		10000000,	/* 100Hz */
		20000000,	/* 50Hz */
		40000000,	/* 25Hz */
		80000000,	/* 12.5Hz */
		160000000,	/* 6.25Hz */
		320000000,	/* 3.125Hz */
		640000000,	/* 1.5625Hz */
	};

	return odr_periods[odr];
}

static int inv_icm45600_set_pwr_mgmt0(struct inv_icm45600_state *st,
				      enum inv_icm45600_sensor_mode gyro,
				      enum inv_icm45600_sensor_mode accel,
				      unsigned int *sleep_ms)
{
	enum inv_icm45600_sensor_mode oldgyro = st->conf.gyro.mode;
	enum inv_icm45600_sensor_mode oldaccel = st->conf.accel.mode;
	unsigned int sleepval;
	unsigned int val;
	int ret;

	/* if nothing changed, exit */
	if (gyro == oldgyro && accel == oldaccel)
		return 0;

	val = FIELD_PREP(INV_ICM45600_PWR_MGMT0_GYRO_MODE_MASK, gyro) |
	      FIELD_PREP(INV_ICM45600_PWR_MGMT0_ACCEL_MODE_MASK, accel);
	ret = regmap_write(st->map, INV_ICM45600_REG_PWR_MGMT0, val);
	if (ret)
		return ret;

	st->conf.gyro.mode = gyro;
	st->conf.accel.mode = accel;

	/* Compute the required wait time for sensors to stabilize. */
	sleepval = 0;

	/* Accel startup time. */
	if (accel != oldaccel && oldaccel == INV_ICM45600_SENSOR_MODE_OFF)
		sleepval = max(sleepval, INV_ICM45600_ACCEL_STARTUP_TIME_MS);

	if (gyro != oldgyro) {
		/* Gyro startup time. */
		if (oldgyro == INV_ICM45600_SENSOR_MODE_OFF)
			sleepval = max(sleepval, INV_ICM45600_GYRO_STARTUP_TIME_MS);
		/* Gyro stop time. */
		else if (gyro == INV_ICM45600_SENSOR_MODE_OFF)
			sleepval = max(sleepval, INV_ICM45600_GYRO_STOP_TIME_MS);
	}

	/* Deferred sleep value if sleep pointer is provided or direct sleep */
	if (sleep_ms)
		*sleep_ms = sleepval;
	else if (sleepval)
		msleep(sleepval);

	return 0;
}

static void inv_icm45600_set_default_conf(struct inv_icm45600_sensor_conf *conf,
					  struct inv_icm45600_sensor_conf *oldconf)
{
	/* Sanitize missing values with current values. */
	if (conf->mode == U8_MAX)
		conf->mode = oldconf->mode;
	if (conf->fs == U8_MAX)
		conf->fs = oldconf->fs;
	if (conf->odr == U8_MAX)
		conf->odr = oldconf->odr;
	if (conf->filter == U8_MAX)
		conf->filter = oldconf->filter;
}

int inv_icm45600_set_accel_conf(struct inv_icm45600_state *st,
				struct inv_icm45600_sensor_conf *conf,
				unsigned int *sleep_ms)
{
	struct inv_icm45600_sensor_conf *oldconf = &st->conf.accel;
	unsigned int val;
	int ret;

	inv_icm45600_set_default_conf(conf, oldconf);

	/* Force the power mode against the ODR when sensor is on. */
	if (conf->mode > INV_ICM45600_SENSOR_MODE_STANDBY) {
		if (conf->odr <= INV_ICM45600_ODR_800HZ_LN) {
			conf->mode = INV_ICM45600_SENSOR_MODE_LOW_NOISE;
		} else {
			conf->mode = INV_ICM45600_SENSOR_MODE_LOW_POWER;
			/* sanitize averaging value depending on ODR for low-power mode */
			/* maximum 1x @400Hz */
			if (conf->odr == INV_ICM45600_ODR_400HZ)
				conf->filter = INV_ICM45600_ACCEL_LP_AVG_SEL_1X;
			else
				conf->filter = INV_ICM45600_ACCEL_LP_AVG_SEL_4X;
		}
	}

	/* Set ACCEL_CONFIG0 register (accel fullscale & odr). */
	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = FIELD_PREP(INV_ICM45600_ACCEL_CONFIG0_FS_MASK, conf->fs) |
		      FIELD_PREP(INV_ICM45600_ACCEL_CONFIG0_ODR_MASK, conf->odr);
		ret = regmap_write(st->map, INV_ICM45600_REG_ACCEL_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	/* Set ACCEL_LP_AVG_SEL register (accel low-power average filter). */
	if (conf->filter != oldconf->filter) {
		ret = regmap_write(st->map, INV_ICM45600_IPREG_SYS2_REG_129,
				   conf->filter);
		if (ret)
			return ret;
		oldconf->filter = conf->filter;
	}

	/* Set PWR_MGMT0 register (accel sensor mode). */
	return inv_icm45600_set_pwr_mgmt0(st, st->conf.gyro.mode, conf->mode,
					  sleep_ms);
}

int inv_icm45600_set_gyro_conf(struct inv_icm45600_state *st,
			       struct inv_icm45600_sensor_conf *conf,
			       unsigned int *sleep_ms)
{
	struct inv_icm45600_sensor_conf *oldconf = &st->conf.gyro;
	unsigned int val;
	int ret;

	inv_icm45600_set_default_conf(conf, oldconf);

	/* Force the power mode against ODR when sensor is on. */
	if (conf->mode > INV_ICM45600_SENSOR_MODE_STANDBY) {
		if (conf->odr >= INV_ICM45600_ODR_6_25HZ_LP) {
			conf->mode = INV_ICM45600_SENSOR_MODE_LOW_POWER;
			conf->filter = INV_ICM45600_GYRO_LP_AVG_SEL_8X;
		} else {
			conf->mode = INV_ICM45600_SENSOR_MODE_LOW_NOISE;
		}
	}

	/* Set GYRO_CONFIG0 register (gyro fullscale & odr). */
	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = FIELD_PREP(INV_ICM45600_GYRO_CONFIG0_FS_MASK, conf->fs) |
		      FIELD_PREP(INV_ICM45600_GYRO_CONFIG0_ODR_MASK, conf->odr);
		ret = regmap_write(st->map, INV_ICM45600_REG_GYRO_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	/* Set GYRO_LP_AVG_SEL register (gyro low-power average filter). */
	if (conf->filter != oldconf->filter) {
		val = FIELD_PREP(INV_ICM45600_IPREG_SYS1_170_GYRO_LP_AVG_MASK, conf->filter);
		ret = regmap_update_bits(st->map, INV_ICM45600_IPREG_SYS1_REG_170,
					 INV_ICM45600_IPREG_SYS1_170_GYRO_LP_AVG_MASK, val);
		if (ret)
			return ret;
		oldconf->filter = conf->filter;
	}

	/* Set PWR_MGMT0 register (gyro sensor mode). */
	return inv_icm45600_set_pwr_mgmt0(st, conf->mode, st->conf.accel.mode,
					  sleep_ms);
}

int inv_icm45600_debugfs_reg(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	int ret;

	guard(mutex)(&st->lock);

	if (readval)
		ret = regmap_read(st->map, reg, readval);
	else
		ret = regmap_write(st->map, reg, writeval);

	return ret;
}

static int inv_icm45600_set_conf(struct inv_icm45600_state *st,
				 const struct inv_icm45600_conf *conf)
{
	unsigned int val;
	int ret;

	/* Set PWR_MGMT0 register (gyro & accel sensor mode, temp enabled). */
	val = FIELD_PREP(INV_ICM45600_PWR_MGMT0_GYRO_MODE_MASK, conf->gyro.mode) |
	      FIELD_PREP(INV_ICM45600_PWR_MGMT0_ACCEL_MODE_MASK, conf->accel.mode);
	ret = regmap_write(st->map, INV_ICM45600_REG_PWR_MGMT0, val);
	if (ret)
		return ret;

	/* Set GYRO_CONFIG0 register (gyro fullscale & odr). */
	val = FIELD_PREP(INV_ICM45600_GYRO_CONFIG0_FS_MASK, conf->gyro.fs) |
	      FIELD_PREP(INV_ICM45600_GYRO_CONFIG0_ODR_MASK, conf->gyro.odr);
	ret = regmap_write(st->map, INV_ICM45600_REG_GYRO_CONFIG0, val);
	if (ret)
		return ret;

	/* Set ACCEL_CONFIG0 register (accel fullscale & odr). */
	val = FIELD_PREP(INV_ICM45600_ACCEL_CONFIG0_FS_MASK, conf->accel.fs) |
	      FIELD_PREP(INV_ICM45600_ACCEL_CONFIG0_ODR_MASK, conf->accel.odr);
	ret = regmap_write(st->map, INV_ICM45600_REG_ACCEL_CONFIG0, val);
	if (ret)
		return ret;

	/* Update the internal configuration. */
	st->conf = *conf;

	return 0;
}

/**
 *  inv_icm45600_setup() - check and setup chip
 *  @st:	driver internal state
 *  @chip_info:	detected chip description
 *  @reset:	define whether a reset is required or not
 *  @bus_setup:	callback for setting up bus specific registers
 *
 *  Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm45600_setup(struct inv_icm45600_state *st,
				const struct inv_icm45600_chip_info *chip_info,
				bool reset, inv_icm45600_bus_setup bus_setup)
{
	const struct device *dev = regmap_get_device(st->map);
	unsigned int val;
	int ret;

	/* Set chip bus configuration if specified. */
	if (bus_setup) {
		ret = bus_setup(st);
		if (ret)
			return ret;
	}

	/* Check chip self-identification value. */
	ret = regmap_read(st->map, INV_ICM45600_REG_WHOAMI, &val);
	if (ret)
		return ret;
	if (val != chip_info->whoami) {
		if (val == U8_MAX || val == 0)
			return dev_err_probe(dev, -ENODEV,
					     "Invalid whoami %#02x expected %#02x (%s)\n",
					     val, chip_info->whoami, chip_info->name);
		else
			dev_warn(dev, "Unexpected whoami %#02x expected %#02x (%s)\n",
				 val, chip_info->whoami, chip_info->name);
	}

	st->chip_info = chip_info;

	if (reset) {
		/* Reset to make sure previous state are not there. */
		ret = regmap_write(st->map, INV_ICM45600_REG_MISC2,
				   INV_ICM45600_MISC2_SOFT_RESET);
		if (ret)
			return ret;
		/* IMU reset time: 1ms. */
		fsleep(1000);

		if (bus_setup) {
			ret = bus_setup(st);
			if (ret)
				return ret;
		}

		ret = regmap_read(st->map, INV_ICM45600_REG_INT_STATUS, &val);
		if (ret)
			return ret;
		if (!(val & INV_ICM45600_INT_STATUS_RESET_DONE)) {
			dev_err(dev, "reset error, reset done bit not set\n");
			return -ENODEV;
		}
	}

	return inv_icm45600_set_conf(st, chip_info->conf);
}

static irqreturn_t inv_icm45600_irq_timestamp(int irq, void *_data)
{
	struct inv_icm45600_state *st = _data;

	st->timestamp.gyro = iio_get_time_ns(st->indio_gyro);
	st->timestamp.accel = iio_get_time_ns(st->indio_accel);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t inv_icm45600_irq_handler(int irq, void *_data)
{
	struct inv_icm45600_state *st = _data;
	struct device *dev = regmap_get_device(st->map);
	unsigned int mask, status;
	int ret;

	guard(mutex)(&st->lock);

	ret = regmap_read(st->map, INV_ICM45600_REG_INT_STATUS, &status);
	if (ret)
		return IRQ_HANDLED;

	/* Read the FIFO data. */
	mask = INV_ICM45600_INT_STATUS_FIFO_THS | INV_ICM45600_INT_STATUS_FIFO_FULL;
	if (status & mask) {
		ret = inv_icm45600_buffer_fifo_read(st);
		if (ret) {
			dev_err(dev, "FIFO read error %d\n", ret);
			return IRQ_HANDLED;
		}
	}

	/* FIFO full warning. */
	if (status & INV_ICM45600_INT_STATUS_FIFO_FULL)
		dev_warn(dev, "FIFO full possible data lost!\n");

	return IRQ_HANDLED;
}

/**
 * inv_icm45600_irq_init() - initialize int pin and interrupt handler
 * @st:		driver internal state
 * @irq:	irq number
 * @irq_type:	irq trigger type
 * @open_drain:	true if irq is open drain, false for push-pull
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm45600_irq_init(struct inv_icm45600_state *st, int irq,
				 int irq_type, bool open_drain)
{
	struct device *dev = regmap_get_device(st->map);
	unsigned int val;
	int ret;

	/* Configure INT1 interrupt: default is active low on edge. */
	switch (irq_type) {
	case IRQF_TRIGGER_RISING:
	case IRQF_TRIGGER_HIGH:
		val = INV_ICM45600_INT1_CONFIG2_ACTIVE_HIGH;
		break;
	default:
		val = INV_ICM45600_INT1_CONFIG2_ACTIVE_LOW;
		break;
	}

	switch (irq_type) {
	case IRQF_TRIGGER_LOW:
	case IRQF_TRIGGER_HIGH:
		val |= INV_ICM45600_INT1_CONFIG2_LATCHED;
		break;
	default:
		break;
	}

	if (!open_drain)
		val |= INV_ICM45600_INT1_CONFIG2_PUSH_PULL;

	ret = regmap_write(st->map, INV_ICM45600_REG_INT1_CONFIG2, val);
	if (ret)
		return ret;

	return devm_request_threaded_irq(dev, irq, inv_icm45600_irq_timestamp,
					 inv_icm45600_irq_handler, irq_type | IRQF_ONESHOT,
					 "inv_icm45600", st);
}

static int inv_icm45600_timestamp_setup(struct inv_icm45600_state *st)
{
	/* Enable timestamps. */
	return regmap_set_bits(st->map, INV_ICM45600_REG_SMC_CONTROL_0,
			       INV_ICM45600_SMC_CONTROL_0_TMST_EN);
}

static int inv_icm45600_enable_regulator_vddio(struct inv_icm45600_state *st)
{
	int ret;

	ret = regulator_enable(st->vddio_supply);
	if (ret)
		return ret;

	/* Wait a little for supply ramp. */
	fsleep(3000);

	return 0;
}

static void inv_icm45600_disable_vddio_reg(void *_data)
{
	struct inv_icm45600_state *st = _data;
	struct device *dev = regmap_get_device(st->map);

	if (pm_runtime_status_suspended(dev))
		return;

	regulator_disable(st->vddio_supply);
}

int inv_icm45600_core_probe(struct regmap *regmap, const struct inv_icm45600_chip_info *chip_info,
				bool reset, inv_icm45600_bus_setup bus_setup)
{
	struct device *dev = regmap_get_device(regmap);
	struct fwnode_handle *fwnode;
	struct inv_icm45600_state *st;
	struct regmap *regmap_custom;
	int irq, irq_type;
	bool open_drain;
	int ret;

	/* Get INT1 only supported interrupt. */
	fwnode = dev_fwnode(dev);
	irq = fwnode_irq_get_byname(fwnode, "int1");
	if (irq < 0)
		return dev_err_probe(dev, irq, "Missing int1 interrupt\n");

	irq_type = irq_get_trigger_type(irq);

	open_drain = device_property_read_bool(dev, "drive-open-drain");

	regmap_custom = devm_regmap_init(dev, &inv_icm45600_regmap_bus,
					 regmap, &inv_icm45600_regmap_config);
	if (IS_ERR(regmap_custom))
		return dev_err_probe(dev, PTR_ERR(regmap_custom), "Failed to register regmap\n");

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return dev_err_probe(dev, -ENOMEM, "Cannot allocate memory\n");

	dev_set_drvdata(dev, st);

	st->fifo.data = devm_kzalloc(dev, 8192, GFP_KERNEL);
	if (!st->fifo.data)
		return dev_err_probe(dev, -ENOMEM, "Cannot allocate fifo memory\n");

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	st->map = regmap_custom;

	ret = iio_read_mount_matrix(dev, &st->orientation);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to retrieve mounting matrix\n");

	st->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(st->vddio_supply))
		return PTR_ERR(st->vddio_supply);

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get vdd regulator\n");

	/* IMU start-up time. */
	fsleep(100000);

	ret = inv_icm45600_enable_regulator_vddio(st);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, inv_icm45600_disable_vddio_reg, st);
	if (ret)
		return ret;

	ret = inv_icm45600_setup(st, chip_info, reset, bus_setup);
	if (ret)
		return ret;

	ret = inv_icm45600_timestamp_setup(st);
	if (ret)
		return ret;

	ret = inv_icm45600_buffer_init(st);
	if (ret)
		return ret;

	ret = inv_icm45600_irq_init(st, irq, irq_type, open_drain);
	if (ret)
		return ret;

	/* Setup runtime power management. */
	ret = devm_pm_runtime_set_active_enabled(dev);
	if (ret)
		return ret;

	pm_runtime_get_noresume(dev);
	/* Suspend after 2 seconds. */
	pm_runtime_set_autosuspend_delay(dev, 2000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put(dev);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(inv_icm45600_core_probe, "IIO_ICM45600");

/*
 * Suspend saves sensors state and turns everything off.
 */
static int inv_icm45600_suspend(struct device *dev)
{
	struct inv_icm45600_state *st = dev_get_drvdata(dev);
	int ret;

	scoped_guard(mutex, &st->lock) {
		/* Disable FIFO data streaming. */
		if (st->fifo.on) {
			unsigned int val;

			/* Clear FIFO_CONFIG3_IF_EN before changing the FIFO configuration */
			ret = regmap_clear_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG3,
						INV_ICM45600_FIFO_CONFIG3_IF_EN);
			if (ret)
				return ret;
			val = FIELD_PREP(INV_ICM45600_FIFO_CONFIG0_MODE_MASK,
					 INV_ICM45600_FIFO_CONFIG0_MODE_BYPASS);
			ret = regmap_update_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG0,
						 INV_ICM45600_FIFO_CONFIG0_MODE_MASK, val);
			if (ret)
				return ret;
		}

		/* Save sensors states */
		st->suspended.gyro = st->conf.gyro.mode;
		st->suspended.accel = st->conf.accel.mode;
	}

	return pm_runtime_force_suspend(dev);
}

/*
 * System resume gets the system back on and restores the sensors state.
 * Manually put runtime power management in system active state.
 */
static int inv_icm45600_resume(struct device *dev)
{
	struct inv_icm45600_state *st = dev_get_drvdata(dev);
	int ret = 0;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	scoped_guard(mutex, &st->lock) {
		/* Restore sensors state. */
		ret = inv_icm45600_set_pwr_mgmt0(st, st->suspended.gyro,
						 st->suspended.accel, NULL);
		if (ret)
			return ret;

		/* Restore FIFO data streaming. */
		if (st->fifo.on) {
			struct inv_icm45600_sensor_state *gyro_st = iio_priv(st->indio_gyro);
			struct inv_icm45600_sensor_state *accel_st = iio_priv(st->indio_accel);
			unsigned int val;

			inv_sensors_timestamp_reset(&gyro_st->ts);
			inv_sensors_timestamp_reset(&accel_st->ts);
			val = FIELD_PREP(INV_ICM45600_FIFO_CONFIG0_MODE_MASK,
					 INV_ICM45600_FIFO_CONFIG0_MODE_STREAM);
			ret = regmap_update_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG0,
						 INV_ICM45600_FIFO_CONFIG0_MODE_MASK, val);
			if (ret)
				return ret;
			/* FIFO_CONFIG3_IF_EN must only be set at end of FIFO the configuration */
			ret = regmap_set_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG3,
					      INV_ICM45600_FIFO_CONFIG3_IF_EN);
		}
	}

	return ret;
}

/* Runtime suspend will turn off sensors that are enabled by iio devices. */
static int inv_icm45600_runtime_suspend(struct device *dev)
{
	struct inv_icm45600_state *st = dev_get_drvdata(dev);
	int ret;

	guard(mutex)(&st->lock);

	/* disable all sensors */
	ret = inv_icm45600_set_pwr_mgmt0(st, INV_ICM45600_SENSOR_MODE_OFF,
					 INV_ICM45600_SENSOR_MODE_OFF, NULL);
	if (ret)
		return ret;

	regulator_disable(st->vddio_supply);

	return 0;
}

/* Sensors are enabled by iio devices, no need to turn them back on here. */
static int inv_icm45600_runtime_resume(struct device *dev)
{
	struct inv_icm45600_state *st = dev_get_drvdata(dev);

	guard(mutex)(&st->lock);

	return inv_icm45600_enable_regulator_vddio(st);
}

EXPORT_NS_GPL_DEV_PM_OPS(inv_icm45600_pm_ops, IIO_ICM45600) = {
	SYSTEM_SLEEP_PM_OPS(inv_icm45600_suspend, inv_icm45600_resume)
	RUNTIME_PM_OPS(inv_icm45600_runtime_suspend,
			   inv_icm45600_runtime_resume, NULL)
};

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-456xx device driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_INV_SENSORS_TIMESTAMP");
