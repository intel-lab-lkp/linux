// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Invensense, Inc.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iio/iio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include "inv_icm45600_buffer.h"
#include "inv_icm45600.h"

static int inv_icm45600_ireg_read(struct regmap *map, unsigned int reg,
				   unsigned int *data, size_t count)
{
	int ret;
	uint8_t addr[2];
	ssize_t i;

	addr[0] = INV_ICM45600_REG_GET_BANK(reg);
	addr[1] = INV_ICM45600_REG_GET_ADDR(reg);

	/* Burst write address */
	ret = regmap_bulk_write(map, INV_ICM45600_REG_IREG_ADDR, addr, 2);
	udelay(INV_ICM45600_IREG_DELAY_US);
	if (ret)
		return ret;

	for (i = 0; i < count; i++) {
		ret = regmap_read(map, INV_ICM45600_REG_IREG_DATA, &data[i]);
		udelay(INV_ICM45600_IREG_DELAY_US);
	}

	return ret;
}

static int inv_icm45600_ireg_write(struct regmap *map, unsigned int reg,
				   uint8_t *data, size_t count)
{
	int ret;
	uint8_t addr_data0[3];
	ssize_t i;

	addr_data0[0] = INV_ICM45600_REG_GET_BANK(reg);
	addr_data0[1] = INV_ICM45600_REG_GET_ADDR(reg);
	addr_data0[2] = data[0];

	/* Burst write address and first byte */
	ret = regmap_bulk_write(map, INV_ICM45600_REG_IREG_ADDR, addr_data0, 3);
	udelay(INV_ICM45600_IREG_DELAY_US);
	if (ret)
		return ret;

	for (i = 1; i < count; i++) {
		ret = regmap_write(map, INV_ICM45600_REG_IREG_DATA, data[i]);
		udelay(INV_ICM45600_IREG_DELAY_US);
	}

	return ret;
}

static int inv_icm45600_read(void *context, const void *reg_buf, size_t reg_size,
			  void *val_buf, size_t val_size)
{
	unsigned int reg = (unsigned int) be16_to_cpup(reg_buf);
	struct regmap *map = context;

	if (INV_ICM45600_REG_GET_BANK(reg) == 0)
		return regmap_bulk_read(map, INV_ICM45600_REG_GET_ADDR(reg), val_buf,
						val_size);

	return inv_icm45600_ireg_read(map, reg, val_buf, val_size);
}

static int inv_icm45600_write(void *context, const void *data,
				   size_t count)
{
	uint8_t *d = (uint8_t *)data;
	unsigned int reg = (unsigned int) be16_to_cpup(data);
	struct regmap *map = context;

	if (INV_ICM45600_REG_GET_BANK(reg) == 0)
		return regmap_bulk_write(map, INV_ICM45600_REG_GET_ADDR(reg),
					d+2, count-2);

	return inv_icm45600_ireg_write(map, reg, d+2, count-2);
}


static const struct regmap_bus inv_icm45600_regmap_bus = {
	.read = inv_icm45600_read,
	.write = inv_icm45600_write,
};

static const struct regmap_config inv_icm45600_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
};

struct inv_icm45600_hw {
	uint8_t whoami;
	const char *name;
	const struct inv_icm45600_conf *conf;
};

/* chip initial default configuration (default FS value is based on icm45686) */
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

static const struct inv_icm45600_hw inv_icm45600_hw[INV_CHIP_NB] = {
	[INV_CHIP_ICM45605] = {
		.whoami = INV_ICM45600_WHOAMI_ICM45605,
		.name = "icm45605",
		.conf = &inv_icm45600_default_conf,
	},
	[INV_CHIP_ICM45686] = {
		.whoami = INV_ICM45600_WHOAMI_ICM45686,
		.name = "icm45686",
		.conf = &inv_icm45686_default_conf,
	},
	[INV_CHIP_ICM45688P] = {
		.whoami = INV_ICM45600_WHOAMI_ICM45688P,
		.name = "icm45688p",
		.conf = &inv_icm45686_default_conf,
	},
	[INV_CHIP_ICM45608] = {
		.whoami = INV_ICM45600_WHOAMI_ICM45608,
		.name = "icm45608",
		.conf = &inv_icm45600_default_conf,
	},
	[INV_CHIP_ICM45634] = {
		.whoami = INV_ICM45600_WHOAMI_ICM45634,
		.name = "icm45634",
		.conf = &inv_icm45600_default_conf,
	},
	[INV_CHIP_ICM45689] = {
		.whoami = INV_ICM45600_WHOAMI_ICM45689,
		.name = "icm45689",
		.conf = &inv_icm45686_default_conf,
	},
	[INV_CHIP_ICM45606] = {
		.whoami = INV_ICM45600_WHOAMI_ICM45606,
		.name = "icm45606",
		.conf = &inv_icm45600_default_conf,
	},
	[INV_CHIP_ICM45687] = {
		.whoami = INV_ICM45600_WHOAMI_ICM45687,
		.name = "icm45687",
		.conf = &inv_icm45686_default_conf,
	},
};

const struct iio_mount_matrix *
inv_icm45600_get_mount_matrix(const struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan)
{
	const struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);

	return &st->orientation;
}

uint32_t inv_icm45600_odr_to_period(enum inv_icm45600_odr odr)
{
	static uint32_t odr_periods[INV_ICM45600_ODR_NB] = {
		/* reserved values */
		0, 0, 0,
		/* 6.4kHz */
		156250,
		/* 3.2kHz */
		312500,
		/* 1.6kHz */
		625000,
		/* 800kHz */
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

	val = INV_ICM45600_PWR_MGMT0_GYRO(gyro) |
	      INV_ICM45600_PWR_MGMT0_ACCEL(accel);
	ret = regmap_write(st->map, INV_ICM45600_REG_PWR_MGMT0, val);
	if (ret)
		return ret;

	st->conf.gyro.mode = gyro;
	st->conf.accel.mode = accel;

	/* compute required wait time for sensors to stabilize */
	sleepval = 0;

	/* accel startup time */
	if (accel != oldaccel && oldaccel == INV_ICM45600_SENSOR_MODE_OFF) {
		if (sleepval < INV_ICM45600_ACCEL_STARTUP_TIME_MS)
			sleepval = INV_ICM45600_ACCEL_STARTUP_TIME_MS;
	}
	if (gyro != oldgyro) {
		/* gyro startup time */
		if (oldgyro == INV_ICM45600_SENSOR_MODE_OFF) {
			if (sleepval < INV_ICM45600_GYRO_STARTUP_TIME_MS)
				sleepval = INV_ICM45600_GYRO_STARTUP_TIME_MS;
		/* gyro stop time */
		} else if (gyro == INV_ICM45600_SENSOR_MODE_OFF) {
			if (sleepval < INV_ICM45600_GYRO_STOP_TIME_MS)
				sleepval =  INV_ICM45600_GYRO_STOP_TIME_MS;
		}
	}

	/* deferred sleep value if sleep pointer is provided or direct sleep */
	if (sleep_ms)
		*sleep_ms = sleepval;
	else if (sleepval)
		msleep(sleepval);

	return 0;
}

int inv_icm45600_set_accel_conf(struct inv_icm45600_state *st,
				struct inv_icm45600_sensor_conf *conf,
				unsigned int *sleep_ms)
{
	struct inv_icm45600_sensor_conf *oldconf = &st->conf.accel;
	unsigned int val;
	int ret;

	/* Sanitize missing values with current values */
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
	case INV_ICM45600_SENSOR_MODE_LOW_POWER:
	case INV_ICM45600_SENSOR_MODE_LOW_NOISE:
		if (conf->odr <= INV_ICM45600_ODR_800HZ_LN) {
			conf->mode = INV_ICM45600_SENSOR_MODE_LOW_NOISE;
		} else if (conf->odr >= INV_ICM45600_ODR_6_25HZ_LP &&
			   conf->odr <= INV_ICM45600_ODR_1_5625HZ_LP) {
			conf->mode = INV_ICM45600_SENSOR_MODE_LOW_POWER;
		}
		break;
	default:
		break;
	}

	/* set ACCEL_CONFIG0 register (accel fullscale & odr) */
	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = INV_ICM45600_ACCEL_CONFIG0_FS(conf->fs) |
		      INV_ICM45600_ACCEL_CONFIG0_ODR(conf->odr);
		ret = regmap_write(st->map, INV_ICM45600_REG_ACCEL_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	/* set ACCEL_LP_AVG_SEL register (accel low-power average filter) */
	if (conf->filter != oldconf->filter) {
		ret = regmap_write(st->map, INV_ICM45600_IPREG_SYS2_REG_129,
			conf->filter);
		if (ret)
			return ret;
		oldconf->filter = conf->filter;
	}

	/* set PWR_MGMT0 register (accel sensor mode) */
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
	case INV_ICM45600_SENSOR_MODE_LOW_POWER:
		if (conf->odr != INV_ICM45600_ODR_400HZ)
			conf->filter = INV_ICM45600_GYRO_LP_AVG_SEL_8X;
		else
			conf->filter = INV_ICM45600_GYRO_LP_AVG_SEL_2X;
		if (conf->odr <= INV_ICM45600_ODR_800HZ_LN)
			conf->mode = INV_ICM45600_SENSOR_MODE_LOW_NOISE;
		break;
	case INV_ICM45600_SENSOR_MODE_LOW_NOISE:
		if (conf->odr >= INV_ICM45600_ODR_6_25HZ_LP &&
			   conf->odr <= INV_ICM45600_ODR_1_5625HZ_LP) {
			conf->mode = INV_ICM45600_SENSOR_MODE_LOW_POWER;
			conf->filter = INV_ICM45600_GYRO_LP_AVG_SEL_8X;
		}
		break;
	default:
		break;
	}

	/* set GYRO_CONFIG0 register (gyro fullscale & odr) */
	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = INV_ICM45600_GYRO_CONFIG0_FS(conf->fs) |
		      INV_ICM45600_GYRO_CONFIG0_ODR(conf->odr);
		ret = regmap_write(st->map, INV_ICM45600_REG_GYRO_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	/* set GYRO_LP_AVG_SEL register (gyro low-power average filter) */
	if (conf->filter != oldconf->filter) {
		ret = regmap_update_bits(st->map, INV_ICM45600_IPREG_SYS1_REG_170,
			INV_ICM45600_IPREG_SYS1_REG_170_MASK, conf->filter);
		if (ret)
			return ret;
		oldconf->filter = conf->filter;
	}

	/* set PWR_MGMT0 register (gyro sensor mode) */
	return inv_icm45600_set_pwr_mgmt0(st, conf->mode, st->conf.accel.mode,
					  sleep_ms);

	return 0;
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

	/* set PWR_MGMT0 register (gyro & accel sensor mode, temp enabled) */
	val = INV_ICM45600_PWR_MGMT0_GYRO(conf->gyro.mode) |
	      INV_ICM45600_PWR_MGMT0_ACCEL(conf->accel.mode);
	ret = regmap_write(st->map, INV_ICM45600_REG_PWR_MGMT0, val);
	if (ret)
		return ret;

	/* set GYRO_CONFIG0 register (gyro fullscale & odr) */
	val = INV_ICM45600_GYRO_CONFIG0_FS(conf->gyro.fs) |
	      INV_ICM45600_GYRO_CONFIG0_ODR(conf->gyro.odr);
	ret = regmap_write(st->map, INV_ICM45600_REG_GYRO_CONFIG0, val);
	if (ret)
		return ret;

	/* set ACCEL_CONFIG0 register (accel fullscale & odr) */
	val = INV_ICM45600_ACCEL_CONFIG0_FS(conf->accel.fs) |
	      INV_ICM45600_ACCEL_CONFIG0_ODR(conf->accel.odr);
	ret = regmap_write(st->map, INV_ICM45600_REG_ACCEL_CONFIG0, val);
	if (ret)
		return ret;

	/* update internal conf */
	st->conf = *conf;

	return 0;
}

/**
 *  inv_icm45600_setup() - check and setup chip
 *  @st:	driver internal state
 *  @bus_setup:	callback for setting up bus specific registers
 *
 *  Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm45600_setup(struct inv_icm45600_state *st, bool reset,
			      inv_icm45600_bus_setup bus_setup)
{
	const struct inv_icm45600_hw *hw = &inv_icm45600_hw[st->chip];
	const struct device *dev = regmap_get_device(st->map);
	unsigned int val;
	int ret;

	/* set chip bus configuration if specified */
	if (bus_setup) {
		ret = bus_setup(st);
		if (ret)
			return ret;
	}

	/* check chip self-identification value */
	ret = regmap_read(st->map, INV_ICM45600_REG_WHOAMI, &val);
	if (ret)
		return ret;
	if (val != hw->whoami) {
		dev_err(dev, "invalid whoami %#02x expected %#02x (%s)\n",
			val, hw->whoami, hw->name);
		return -ENODEV;
	}
	st->name = hw->name;

	if (reset) {
		/* reset to make sure previous state are not there */
		ret = regmap_write(st->map, INV_ICM45600_REG_MISC2,
				INV_ICM45600_MISC2_SOFT_RESET);
		if (ret)
			return ret;
		msleep(INV_ICM45600_RESET_TIME_MS);

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

	return inv_icm45600_set_conf(st, hw->conf);
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

	/* read FIFO data */
	mask = INV_ICM45600_INT_STATUS_FIFO_THS | INV_ICM45600_INT_STATUS_FIFO_FULL;
	if (status & mask) {
		ret = inv_icm45600_buffer_fifo_read(st, 0);
		if (ret) {
			dev_err(dev, "FIFO read error %d\n", ret);
			return IRQ_HANDLED;
		}
		ret = inv_icm45600_buffer_fifo_parse(st);
		if (ret)
			dev_err(dev, "FIFO parsing error %d\n", ret);
	}

	/* FIFO full warning */
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

	/* configure INT1 interrupt: default is active low on edge */
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

	irq_type |= IRQF_ONESHOT;
	return devm_request_threaded_irq(dev, irq, inv_icm45600_irq_timestamp,
					 inv_icm45600_irq_handler, irq_type,
					 "inv_icm45600", st);
}

static int inv_icm45600_timestamp_setup(struct inv_icm45600_state *st)
{
	/* enable timestamp */
	return regmap_set_bits(st->map, INV_ICM45600_REG_SMC_CONTROL_0,
					INV_ICM45600_SMC_CONTROL_0_TMST_EN);
}

static int inv_icm45600_enable_regulator_vddio(struct inv_icm45600_state *st)
{
	int ret;

	ret = regulator_enable(st->vddio_supply);
	if (ret)
		return ret;

	/* wait a little for supply ramp */
	usleep_range(3000, 4000);

	return 0;
}

static void inv_icm45600_disable_vdd_reg(void *_data)
{
	struct inv_icm45600_state *st = _data;
	const struct device *dev = regmap_get_device(st->map);
	int ret;

	ret = regulator_disable(st->vdd_supply);
	if (ret)
		dev_err(dev, "failed to disable vdd error %d\n", ret);
}

static void inv_icm45600_disable_vddio_reg(void *_data)
{
	struct inv_icm45600_state *st = _data;
	const struct device *dev = regmap_get_device(st->map);
	int ret;

	ret = regulator_disable(st->vddio_supply);
	if (ret)
		dev_err(dev, "failed to disable vddio error %d\n", ret);
}

static void inv_icm45600_disable_pm(void *_data)
{
	struct device *dev = _data;

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
}

int inv_icm45600_core_probe(struct regmap *regmap, int chip, bool reset,
			    inv_icm45600_bus_setup bus_setup)
{
	struct device *dev = regmap_get_device(regmap);
	struct fwnode_handle *fwnode;
	struct inv_icm45600_state *st;
	struct regmap *regmap_custom;
	int irq, irq_type;
	bool open_drain;
	int ret;

	if (chip <= INV_CHIP_INVALID || chip >= INV_CHIP_NB) {
		dev_err(dev, "invalid chip = %d\n", chip);
		return -ENODEV;
	}

	/* get INT1 only supported interrupt */
	fwnode = dev_fwnode(dev);
	if (!fwnode)
		return -ENODEV;
	irq = fwnode_irq_get_byname(fwnode, "INT1");
	if (irq < 0) {
		if (irq != -EPROBE_DEFER)
			dev_err(dev, "error missing INT1 interrupt\n");
		return irq;
	}

	irq_type = irq_get_trigger_type(irq);
	if (!irq_type)
		irq_type = IRQF_TRIGGER_FALLING;

	open_drain = device_property_read_bool(dev, "drive-open-drain");

	regmap_custom = devm_regmap_init(dev, &inv_icm45600_regmap_bus,
					 regmap, &inv_icm45600_regmap_config);
	if (IS_ERR(regmap_custom)) {
		dev_err(dev, "Failed to register icm45600 regmap %ld\n", PTR_ERR(regmap_custom));
		return PTR_ERR(regmap_custom);
	}

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	dev_set_drvdata(dev, st);
	mutex_init(&st->lock);
	st->chip = chip;
	st->map = regmap_custom;

	ret = iio_read_mount_matrix(dev, &st->orientation);
	if (ret) {
		dev_err(dev, "failed to retrieve mounting matrix %d\n", ret);
		return ret;
	}

	st->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(st->vdd_supply))
		return PTR_ERR(st->vdd_supply);

	st->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(st->vddio_supply))
		return PTR_ERR(st->vddio_supply);

	ret = regulator_enable(st->vdd_supply);
	if (ret)
		return ret;
	msleep(INV_ICM45600_POWER_UP_TIME_MS);

	ret = devm_add_action_or_reset(dev, inv_icm45600_disable_vdd_reg, st);
	if (ret)
		return ret;

	ret = inv_icm45600_enable_regulator_vddio(st);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, inv_icm45600_disable_vddio_reg, st);
	if (ret)
		return ret;

	/* setup chip registers */
	ret = inv_icm45600_setup(st, reset, bus_setup);
	if (ret)
		return ret;

	ret = inv_icm45600_timestamp_setup(st);
	if (ret)
		return ret;

	ret = inv_icm45600_buffer_init(st);
	if (ret)
		return ret;

	st->indio_gyro = inv_icm45600_gyro_init(st);
	if (IS_ERR(st->indio_gyro))
		return PTR_ERR(st->indio_gyro);

	st->indio_accel = inv_icm45600_accel_init(st);
	if (IS_ERR(st->indio_accel))
		return PTR_ERR(st->indio_accel);

	ret = inv_icm45600_irq_init(st, irq, irq_type, open_drain);
	if (ret)
		return ret;

	/* setup runtime power management */
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, INV_ICM45600_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put(dev);

	return devm_add_action_or_reset(dev, inv_icm45600_disable_pm, dev);
}
EXPORT_SYMBOL_NS_GPL(inv_icm45600_core_probe, "IIO_ICM45600");

/*
 * Suspend saves sensors state and turns everything off.
 * Check first if runtime suspend has not already done the job.
 */
static int inv_icm45600_suspend(struct device *dev)
{
	struct inv_icm45600_state *st = dev_get_drvdata(dev);
	int ret;

	guard(mutex)(&st->lock);

	st->suspended.gyro = st->conf.gyro.mode;
	st->suspended.accel = st->conf.accel.mode;
	if (pm_runtime_suspended(dev))
		return 0;

	/* disable FIFO data streaming */
	if (st->fifo.on) {
		ret = regmap_clear_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG3,
					INV_ICM45600_FIFO_CONFIG3_IF_EN);
		if (ret)
			return ret;
		ret = regmap_update_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG0,
					 INV_ICM45600_FIFO_CONFIG0_MODE_MASK,
					 INV_ICM45600_FIFO_CONFIG0_MODE_BYPASS);
		if (ret)
			return ret;
	}

	ret = inv_icm45600_set_pwr_mgmt0(st, INV_ICM45600_SENSOR_MODE_OFF,
					 INV_ICM45600_SENSOR_MODE_OFF, NULL);
	if (ret)
		return ret;

	regulator_disable(st->vddio_supply);

	return ret;
}

/*
 * System resume gets the system back on and restores the sensors state.
 * Manually put runtime power management in system active state.
 */
static int inv_icm45600_resume(struct device *dev)
{
	struct inv_icm45600_state *st = dev_get_drvdata(dev);
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(st->indio_gyro);
	struct inv_icm45600_sensor_state *accel_st = iio_priv(st->indio_accel);
	int ret;

	guard(mutex)(&st->lock);

	ret = inv_icm45600_enable_regulator_vddio(st);
	if (ret)
		return ret;

	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	/* restore sensors state */
	ret = inv_icm45600_set_pwr_mgmt0(st, st->suspended.gyro,
					 st->suspended.accel, NULL);
	if (ret)
		return ret;

	/* restore FIFO data streaming */
	if (st->fifo.on) {
		inv_sensors_timestamp_reset(&gyro_st->ts);
		inv_sensors_timestamp_reset(&accel_st->ts);
		ret = regmap_update_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG0,
					 INV_ICM45600_FIFO_CONFIG0_MODE_MASK,
					 INV_ICM45600_FIFO_CONFIG0_MODE_STREAM);
		if (ret)
			return ret;
		ret = regmap_set_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG3,
				      INV_ICM45600_FIFO_CONFIG3_IF_EN);
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

	return ret;
}

/* Sensors are enabled by iio devices, no need to turn them back on here. */
static int inv_icm45600_runtime_resume(struct device *dev)
{
	struct inv_icm45600_state *st = dev_get_drvdata(dev);
	int ret;

	guard(mutex)(&st->lock);

	ret = inv_icm45600_enable_regulator_vddio(st);

	return ret;
}


EXPORT_NS_GPL_DEV_PM_OPS(inv_icm45600_pm_ops, IIO_ICM45600) = {
	SET_SYSTEM_SLEEP_PM_OPS(inv_icm45600_suspend, inv_icm45600_resume)
	SET_RUNTIME_PM_OPS(inv_icm45600_runtime_suspend,
			   inv_icm45600_runtime_resume, NULL)
};

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-456xx device driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_INV_SENSORS_TIMESTAMP");
