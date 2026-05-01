// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 InvenSense, Inc.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iio/iio.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include "inv_icm42607.h"
#include "inv_icm42607_buffer.h"

static const struct regmap_range_cfg inv_icm42607_regmap_ranges[] = {
	{
		.name = "user bank",
		.range_min = 0x0000,
		.range_max = 0x00FF,
		.window_start = 0,
		.window_len = 0x0100,
	},
};

const struct regmap_config inv_icm42607_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x00FF,
	.ranges = inv_icm42607_regmap_ranges,
	.num_ranges = ARRAY_SIZE(inv_icm42607_regmap_ranges),
	.cache_type = REGCACHE_NONE,
};
EXPORT_SYMBOL_NS_GPL(inv_icm42607_regmap_config, "IIO_ICM42607");

struct inv_icm42607_hw {
	uint8_t whoami;
	const char *name;
	const struct inv_icm42607_conf *conf;
};

/* chip initial default configuration */
static const struct inv_icm42607_conf inv_icm42607_default_conf = {
	.gyro = {
		.mode = INV_ICM42607_SENSOR_MODE_OFF,
		.fs = INV_ICM42607_GYRO_FS_1000DPS,
		.odr = INV_ICM42607_ODR_100HZ,
		.filter = INV_ICM42607_FILTER_BW_25HZ,
	},
	.accel = {
		.mode = INV_ICM42607_SENSOR_MODE_OFF,
		.fs = INV_ICM42607_ACCEL_FS_4G,
		.odr = INV_ICM42607_ODR_100HZ,
		.filter = INV_ICM42607_FILTER_BW_25HZ,
	},
	.temp_en = false,
};

static const struct inv_icm42607_hw inv_icm42607_hw[INV_CHIP_NB] = {
	[INV_CHIP_ICM42607] = {
		.whoami = INV_ICM42607_WHOAMI,
		.name = "icm42607",
		.conf = &inv_icm42607_default_conf,
	},
	[INV_CHIP_ICM42607P] = {
		.whoami = INV_ICM42607P_WHOAMI,
		.name = "icm42607p",
		.conf = &inv_icm42607_default_conf,
	},
};

const struct iio_mount_matrix *
inv_icm42607_get_mount_matrix(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan)
{
	const struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);

	return &st->orientation;
}

u32 inv_icm42607_odr_to_period(enum inv_icm42607_odr odr)
{
	static const u32 odr_periods[INV_ICM42607_ODR_NB] = {
		/* Reserved values */
		0, 0, 0, 0, 0,
		/* 1600Hz */
		625000,
		/* 800Hz */
		1250000,
		/* 400Hz */
		2500000,
		/* 200Hz */
		5000000,
		/* 100 Hz */
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

	odr = clamp(odr, INV_ICM42607_ODR_1600HZ, INV_ICM42607_ODR_1_5625HZ_LP);

	return odr_periods[odr];
}

static int inv_icm42607_set_pwr_mgmt0(struct inv_icm42607_state *st,
				      enum inv_icm42607_sensor_mode gyro,
				      enum inv_icm42607_sensor_mode accel,
				      bool temp, unsigned int *sleep_ms)
{
	enum inv_icm42607_sensor_mode oldgyro = st->conf.gyro.mode;
	enum inv_icm42607_sensor_mode oldaccel = st->conf.accel.mode;
	bool oldtemp = st->conf.temp_en;
	unsigned int sleepval;
	unsigned int val;
	int ret;

	if (gyro == oldgyro && accel == oldaccel && temp == oldtemp)
		return 0;

	val = FIELD_PREP(INV_ICM42607_PWR_MGMT0_GYRO_MODE_MASK, gyro);
	val |= FIELD_PREP(INV_ICM42607_PWR_MGMT0_ACCEL_MODE_MASK, accel);
	if (!temp)
		val |= INV_ICM42607_PWR_MGMT0_ACCEL_LP_CLK_SEL;
	ret = regmap_write(st->map, INV_ICM42607_REG_PWR_MGMT0, val);
	if (ret)
		return ret;

	st->conf.gyro.mode = gyro;
	st->conf.accel.mode = accel;
	st->conf.temp_en = temp;

	sleepval = 0;
	if (temp && !oldtemp) {
		if (sleepval < INV_ICM42607_TEMP_STARTUP_TIME_MS)
			sleepval = INV_ICM42607_TEMP_STARTUP_TIME_MS;
	}
	if (accel != oldaccel && oldaccel == INV_ICM42607_SENSOR_MODE_OFF) {
		usleep_range(200, 300);
		if (sleepval < INV_ICM42607_ACCEL_STARTUP_TIME_MS)
			sleepval = INV_ICM42607_ACCEL_STARTUP_TIME_MS;
	}
	if (gyro != oldgyro) {
		if (oldgyro == INV_ICM42607_SENSOR_MODE_OFF) {
			usleep_range(200, 300);
			if (sleepval < INV_ICM42607_GYRO_STARTUP_TIME_MS)
				sleepval = INV_ICM42607_GYRO_STARTUP_TIME_MS;
		} else if (gyro == INV_ICM42607_SENSOR_MODE_OFF) {
			if (sleepval < INV_ICM42607_GYRO_STOP_TIME_MS)
				sleepval = INV_ICM42607_GYRO_STOP_TIME_MS;
		}
	}

	if (sleep_ms)
		*sleep_ms = sleepval;
	else if (sleepval)
		msleep(sleepval);

	return 0;
}

int inv_icm42607_set_accel_conf(struct inv_icm42607_state *st,
				struct inv_icm42607_sensor_conf *conf,
				unsigned int *sleep_ms)
{
	struct inv_icm42607_sensor_conf *oldconf = &st->conf.accel;
	unsigned int val;
	int ret;

	if (conf->mode < 0)
		conf->mode = oldconf->mode;
	if (conf->fs < 0)
		conf->fs = oldconf->fs;
	if (conf->odr < 0)
		conf->odr = oldconf->odr;
	if (conf->filter < 0)
		conf->filter = oldconf->filter;

	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = FIELD_PREP(INV_ICM42607_ACCEL_CONFIG0_FS_SEL_MASK, conf->fs);
		val |= FIELD_PREP(INV_ICM42607_ACCEL_CONFIG0_ODR_MASK, conf->odr);
		ret = regmap_write(st->map, INV_ICM42607_REG_ACCEL_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	if (conf->filter != oldconf->filter) {
		if (conf->mode == INV_ICM42607_SENSOR_MODE_LOW_POWER) {
			val = FIELD_PREP(INV_ICM42607_ACCEL_CONFIG1_AVG_MASK, conf->filter);
			ret = regmap_update_bits(st->map, INV_ICM42607_REG_ACCEL_CONFIG1,
						 INV_ICM42607_ACCEL_CONFIG1_AVG_MASK, val);
		} else {
			val = FIELD_PREP(INV_ICM42607_ACCEL_CONFIG1_FILTER_MASK,
					 conf->filter);
			ret = regmap_update_bits(st->map, INV_ICM42607_REG_ACCEL_CONFIG1,
						 INV_ICM42607_ACCEL_CONFIG1_FILTER_MASK, val);
		}
		if (ret)
			return ret;
		oldconf->filter = conf->filter;
	}

	return inv_icm42607_set_pwr_mgmt0(st, st->conf.gyro.mode, conf->mode,
					  st->conf.temp_en, sleep_ms);
}

int inv_icm42607_set_temp_conf(struct inv_icm42607_state *st, bool enable,
			       unsigned int *sleep_ms)
{
	unsigned int val;
	int ret;

	val = FIELD_PREP(INV_ICM42607_TEMP_CONFIG0_FILTER_MASK,
			 INV_ICM42607_FILTER_BW_34HZ);
	ret = regmap_update_bits(st->map, INV_ICM42607_REG_TEMP_CONFIG0,
				 INV_ICM42607_TEMP_CONFIG0_FILTER_MASK, val);
	if (ret)
		return ret;

	return inv_icm42607_set_pwr_mgmt0(st, st->conf.gyro.mode,
					  st->conf.accel.mode, enable,
					  sleep_ms);
}

int inv_icm42607_enable_wom(struct inv_icm42607_state *st)
{
	int ret;

	/* enable WoM hardware */
	ret = regmap_write(st->map, INV_ICM42607_REG_WOM_CONFIG,
			   FIELD_PREP(INV_ICM42607_WOM_CONFIG_INT_DUR_MASK, 1) |
			   INV_ICM42607_WOM_CONFIG_MODE |
			   INV_ICM42607_WOM_CONFIG_EN);
	if (ret)
		return ret;

	/* enable WoM interrupt */
	return regmap_set_bits(st->map, INV_ICM42607_REG_INT_SOURCE1,
			       INV_ICM42607_INT_SOURCE1_WOM_INT1_EN);
}

int inv_icm42607_disable_wom(struct inv_icm42607_state *st)
{
	int ret;

	/* disable WoM interrupt */
	ret = regmap_clear_bits(st->map, INV_ICM42607_REG_INT_SOURCE1,
				INV_ICM42607_INT_SOURCE1_WOM_INT1_EN);
	if (ret)
		return ret;

	/* disable WoM hardware */
	return regmap_clear_bits(st->map, INV_ICM42607_REG_WOM_CONFIG,
				 INV_ICM42607_WOM_CONFIG_EN);
}


int inv_icm42607_debugfs_reg(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);

	guard(mutex)(&st->lock);

	if (readval)
		return regmap_read(st->map, reg, readval);

	return regmap_write(st->map, reg, writeval);
}

static int inv_icm42607_set_conf(struct inv_icm42607_state *st,
				 const struct inv_icm42607_conf *conf)
{
	unsigned int val;
	int ret;

	val = FIELD_PREP(INV_ICM42607_PWR_MGMT0_GYRO_MODE_MASK,
			 conf->gyro.mode);
	val |= FIELD_PREP(INV_ICM42607_PWR_MGMT0_ACCEL_MODE_MASK,
			  conf->accel.mode);
	/*
	 * No temperature enable reg in datasheet, but BSP driver
	 * selected RC oscillator clock in LP mode when temperature
	 * was disabled.
	 */
	if (!conf->temp_en)
		val |= INV_ICM42607_PWR_MGMT0_ACCEL_LP_CLK_SEL;
	ret = regmap_write(st->map, INV_ICM42607_REG_PWR_MGMT0, val);
	if (ret)
		return ret;

	val = FIELD_PREP(INV_ICM42607_GYRO_CONFIG0_FS_SEL_MASK,
			 conf->gyro.fs);
	val |= FIELD_PREP(INV_ICM42607_GYRO_CONFIG0_ODR_MASK,
			  conf->gyro.odr);
	ret = regmap_write(st->map, INV_ICM42607_REG_GYRO_CONFIG0, val);
	if (ret)
		return ret;

	val = FIELD_PREP(INV_ICM42607_ACCEL_CONFIG0_FS_SEL_MASK, conf->accel.fs);
	val |= FIELD_PREP(INV_ICM42607_ACCEL_CONFIG0_ODR_MASK, conf->accel.odr);
	ret = regmap_write(st->map, INV_ICM42607_REG_ACCEL_CONFIG0, val);
	if (ret)
		return ret;

	val = FIELD_PREP(INV_ICM42607_GYRO_CONFIG1_FILTER_MASK,
			 conf->gyro.filter);
	ret = regmap_write(st->map, INV_ICM42607_REG_GYRO_CONFIG1, val);
	if (ret)
		return ret;

	val = FIELD_PREP(INV_ICM42607_ACCEL_CONFIG1_FILTER_MASK,
			 conf->accel.filter);
	ret = regmap_write(st->map, INV_ICM42607_REG_ACCEL_CONFIG1, val);
	if (ret)
		return ret;

	st->conf = *conf;

	return 0;
}

/**
 *  inv_icm42607_setup() - check and setup chip
 *  @st:	driver internal state
 *  @bus_setup:	callback for setting up bus specific registers
 *
 *  Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm42607_setup(struct inv_icm42607_state *st,
			      inv_icm42607_bus_setup bus_setup)
{
	const struct inv_icm42607_hw *hw = &inv_icm42607_hw[st->chip];
	const struct device *dev = regmap_get_device(st->map);
	unsigned int val;
	int ret;

	ret = regmap_read(st->map, INV_ICM42607_REG_WHOAMI, &val);
	if (ret)
		return ret;

	if (val != hw->whoami)
		dev_warn_probe(dev, -ENODEV,
			       "invalid whoami %#02x expected %#02x (%s)\n",
			       val, hw->whoami, hw->name);

	st->name = hw->name;

	ret = regmap_write(st->map, INV_ICM42607_REG_SIGNAL_PATH_RESET,
			   INV_ICM42607_SIGNAL_PATH_RESET_SOFT_RESET);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(st->map, INV_ICM42607_REG_INT_STATUS,
				       val, val & INV_ICM42607_INT_STATUS_RESET_DONE,
				       INV_ICM42607_RESET_TIME_MS * 100,
				       INV_ICM42607_RESET_TIME_MS * 1000);

	if (ret)
		return dev_err_probe(dev, ret,
				     "reset error, reset done bit not set\n");

	ret = bus_setup(st);
	if (ret)
		return ret;

	ret = regmap_set_bits(st->map, INV_ICM42607_REG_INTF_CONFIG0,
			      INV_ICM42607_INTF_CONFIG0_SENSOR_DATA_ENDIAN);
	if (ret)
		return ret;

	ret = regmap_update_bits(st->map, INV_ICM42607_REG_INTF_CONFIG1,
				 INV_ICM42607_INTF_CONFIG1_CLKSEL_MASK,
				 INV_ICM42607_INTF_CONFIG1_CLKSEL_PLL);
	if (ret)
		return ret;

	return inv_icm42607_set_conf(st, hw->conf);
}

static irqreturn_t inv_icm42607_irq_timestamp(int irq, void *_data)
{
	struct inv_icm42607_state *st = _data;

	st->timestamp.gyro = iio_get_time_ns(st->indio_gyro);
	st->timestamp.accel = iio_get_time_ns(st->indio_accel);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t inv_icm42607_irq_handler(int irq, void *_data)
{
	struct inv_icm42607_state *st = _data;
	struct device *dev = regmap_get_device(st->map);
	unsigned int status;
	int ret;

	mutex_lock(&st->lock);

	if (st->apex.on) {
		unsigned int status2, status3;

		/* read INT_STATUS2 and INT_STATUS3 in 1 operation */
		ret = regmap_bulk_read(st->map, INV_ICM42607_REG_INT_STATUS2, st->buffer, 2);
		if (ret)
			goto out_unlock;
		status2 = st->buffer[0];
		status3 = st->buffer[1];
		inv_icm42607_accel_handle_events(st->indio_accel, status2, status3,
						 st->timestamp.accel);
	}

	ret = regmap_read(st->map, INV_ICM42607_REG_INT_STATUS, &status);
	if (ret)
		goto out_unlock;

	if (status & INV_ICM42607_INT_STATUS_FIFO_FULL)
		dev_warn(dev, "FIFO full data lost!\n");

	if (status & INV_ICM42607_INT_STATUS_FIFO_THS) {
		ret = inv_icm42607_buffer_fifo_read(st, 0);
		if (ret) {
			dev_err(dev, "FIFO read error %d\n", ret);
			goto out_unlock;
		}
		ret = inv_icm42607_buffer_fifo_parse(st);
		if (ret)
			dev_err(dev, "FIFO parsing error %d\n", ret);
	}

out_unlock:
	mutex_unlock(&st->lock);
	return IRQ_HANDLED;
}

/**
 * inv_icm42607_irq_init() - initialize int pin and interrupt handler
 * @st:		driver internal state
 * @irq:	irq number
 * @irq_type:	irq trigger type
 * @open_drain:	true if irq is open drain, false for push-pull
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm42607_irq_init(struct inv_icm42607_state *st, int irq,
				int irq_type, bool open_drain)
{
	struct device *dev = regmap_get_device(st->map);
	unsigned int val = 0;
	int ret;

	switch (irq_type) {
	case IRQF_TRIGGER_RISING:
	case IRQF_TRIGGER_HIGH:
		val = INV_ICM42607_INT_CONFIG_INT1_ACTIVE_HIGH;
		break;
	default:
		val = INV_ICM42607_INT_CONFIG_INT1_ACTIVE_LOW;
		break;
	}

	switch (irq_type) {
	case IRQF_TRIGGER_LOW:
	case IRQF_TRIGGER_HIGH:
		val |= INV_ICM42607_INT_CONFIG_INT1_LATCHED;
		break;
	default:
		break;
	}

	if (!open_drain)
		val |= INV_ICM42607_INT_CONFIG_INT1_PUSH_PULL;

	ret = regmap_write(st->map, INV_ICM42607_REG_INT_CONFIG, val);
	if (ret)
		return ret;

	irq_type |= IRQF_ONESHOT;
	return devm_request_threaded_irq(dev, irq, inv_icm42607_irq_timestamp,
					 inv_icm42607_irq_handler, irq_type,
					 st->name, st);
}

static int inv_icm42607_enable_vddio_reg(struct inv_icm42607_state *st)
{
	int ret;

	ret = regulator_enable(st->vddio_supply);
	if (ret)
		return ret;

	fsleep(INV_ICM42607_POWER_UP_TIME_US);

	return 0;
}

static void inv_icm42607_disable_vddio_reg(void *_data)
{
	struct inv_icm42607_state *st = _data;

	regulator_disable(st->vddio_supply);
}

int inv_icm42607_core_probe(struct regmap *regmap, int chip,
			    inv_icm42607_bus_setup bus_setup)
{
	struct device *dev = regmap_get_device(regmap);
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct inv_icm42607_state *st;
	int irq, irq_type;
	bool open_drain;
	int ret;

	/*
	 * Keep bounds checking in case more chips are added, for now only
	 * 2 are supported.
	 */
	if (chip < INV_CHIP_INVALID || chip >= INV_CHIP_NB)
		dev_warn_probe(dev, -ENODEV, "Invalid chip = %d\n", chip);

	irq = fwnode_irq_get_byname(fwnode, "INT1");
	if (irq < 0)
		return dev_err_probe(dev, irq, "error missing INT1 interrupt\n");

	irq_type = irq_get_trigger_type(irq);
	if (!irq_type)
		irq_type = IRQF_TRIGGER_FALLING;
	open_drain = device_property_read_bool(dev, "drive-open-drain");

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	dev_set_drvdata(dev, st);

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	st->chip = chip;
	st->map = regmap;
	st->irq = irq;

	ret = iio_read_mount_matrix(dev, &st->orientation);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to retrieve mounting matrix %d\n", ret);

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get vdd regulator\n");

	st->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(st->vddio_supply))
		return PTR_ERR(st->vddio_supply);

	ret = inv_icm42607_enable_vddio_reg(st);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, inv_icm42607_disable_vddio_reg, st);
	if (ret)
		return ret;

	/* Setup chip registers (includes WHOAMI check, reset check, bus setup) */
	ret = inv_icm42607_setup(st, bus_setup);
	if (ret)
		return ret;

	/* Initialize buffer/FIFO handling */
	ret = inv_icm42607_buffer_init(st);
	if (ret)
		return ret;

	/* Initialize IIO device for Accel */
	st->indio_accel = inv_icm42607_accel_init(st);
	if (IS_ERR(st->indio_accel))
		return PTR_ERR(st->indio_accel);

	/* Initialize interrupt handling */
	ret = inv_icm42607_irq_init(st, irq, irq_type, open_drain);
	if (ret)
		return ret;

	ret = devm_pm_runtime_set_active_enabled(dev);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, INV_ICM42607_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(inv_icm42607_core_probe, "IIO_ICM42607");

/*
 * Suspend saves sensors state and turns everything off.
 * Check first if runtime suspend has not already done the job.
 */
static int inv_icm42607_suspend(struct device *dev)
{
	struct inv_icm42607_state *st = dev_get_drvdata(dev);
	struct device *accel_dev;
	bool wakeup;
	int accel_conf;
	int ret = 0;

	guard(mutex)(&st->lock);

	st->suspended.gyro = st->conf.gyro.mode;
	st->suspended.accel = st->conf.accel.mode;
	st->suspended.temp = st->conf.temp_en;
	if (pm_runtime_suspended(dev))
		return 0;

	if (st->fifo.on) {
		ret = regmap_write(st->map, INV_ICM42607_REG_FIFO_CONFIG1,
				   INV_ICM42607_FIFO_CONFIG1_BYPASS);
		if (ret)
			return ret;
	}

	/* keep chip on and wake-up capable if APEX and wakeup on */
	accel_dev = &st->indio_accel->dev;
	wakeup = st->apex.on && device_may_wakeup(accel_dev);
	if (wakeup) {
		/* keep accel on and setup irq for wakeup */
		accel_conf = st->conf.accel.mode;
		enable_irq_wake(st->irq);
		disable_irq(st->irq);
	} else {
		/* disable APEX features and accel if wakeup disabled */
		if (st->apex.wom.enable) {
			ret = inv_icm42607_disable_wom(st);
			if (ret)
				return ret;
		}
		accel_conf = INV_ICM42607_SENSOR_MODE_OFF;
	}

	ret = inv_icm42607_set_pwr_mgmt0(st, INV_ICM42607_SENSOR_MODE_OFF,
					 INV_ICM42607_SENSOR_MODE_OFF, false,
					 NULL);
	if (ret)
		return ret;

	if (!wakeup)
		regulator_disable(st->vddio_supply);

	return 0;
}

/*
 * System resume gets the system back on and restores the sensors state.
 * Manually put runtime power management in system active state.
 */
static int inv_icm42607_resume(struct device *dev)
{
	struct inv_icm42607_state *st = dev_get_drvdata(dev);
	struct inv_icm42607_sensor_state *gyro_st = iio_priv(st->indio_gyro);
	struct inv_icm42607_sensor_state *accel_st = iio_priv(st->indio_accel);
	struct device *accel_dev;
	bool wakeup;
	int ret;

	guard(mutex)(&st->lock);

	if (pm_runtime_suspended(dev))
		return 0;

	/* check wakeup capability */
	accel_dev = &st->indio_accel->dev;
	wakeup = st->apex.on && device_may_wakeup(accel_dev);
	/* restore irq state */
	if (wakeup) {
		enable_irq(st->irq);
		disable_irq_wake(st->irq);
	} else {
		ret = inv_icm42607_enable_vddio_reg(st);
		if (ret)
			return ret;
	}

	/* restore sensors state */
	ret = inv_icm42607_set_pwr_mgmt0(st, st->suspended.gyro,
					 st->suspended.accel,
					 st->suspended.temp, NULL);
	if (ret)
		return ret;

	/* restore APEX features if disabled */
	if (!wakeup && st->apex.wom.enable) {
		ret = inv_icm42607_enable_wom(st);
		if (ret)
			return ret;
	}

	if (st->fifo.on) {
		inv_sensors_timestamp_reset(&gyro_st->ts);
		inv_sensors_timestamp_reset(&accel_st->ts);
		ret = regmap_write(st->map, INV_ICM42607_REG_FIFO_CONFIG1,
				   INV_ICM42607_FIFO_CONFIG1_MODE);
	}

	return ret;
}

static int inv_icm42607_runtime_suspend(struct device *dev)
{
	struct inv_icm42607_state *st = dev_get_drvdata(dev);
	int ret = 0;

	guard(mutex)(&st->lock);

	ret = inv_icm42607_set_pwr_mgmt0(st, INV_ICM42607_SENSOR_MODE_OFF,
					 INV_ICM42607_SENSOR_MODE_OFF, false,
					 NULL);
	if (ret)
		return ret;

	regulator_disable(st->vddio_supply);

	return 0;
}

static int inv_icm42607_runtime_resume(struct device *dev)
{
	struct inv_icm42607_state *st = dev_get_drvdata(dev);

	guard(mutex)(&st->lock);

	return inv_icm42607_enable_vddio_reg(st);
}

EXPORT_NS_GPL_DEV_PM_OPS(inv_icm42607_pm_ops, IIO_ICM42607) = {
	SYSTEM_SLEEP_PM_OPS(inv_icm42607_suspend, inv_icm42607_resume)
	RUNTIME_PM_OPS(inv_icm42607_runtime_suspend,
		       inv_icm42607_runtime_resume, NULL)
};

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-42607x device driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_INV_SENSORS_TIMESTAMP");
