// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 InvenSense, Inc.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/regulator/consumer.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>

#include "inv_icm42607.h"

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

u32 inv_icm42607_odr_to_period(enum inv_icm42607_odr odr)
{
	static u32 odr_periods[INV_ICM42607_ODR_NB] = {
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

	return odr_periods[odr];
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

	val = INV_ICM42607_PWR_MGMT0_GYRO(conf->gyro.mode) |
	INV_ICM42607_PWR_MGMT0_ACCEL(conf->accel.mode);
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

	val = INV_ICM42607_GYRO_CONFIG0_FS_SEL(conf->gyro.fs) |
	INV_ICM42607_GYRO_CONFIG0_ODR(conf->gyro.odr);
	ret = regmap_write(st->map, INV_ICM42607_REG_GYRO_CONFIG0, val);
	if (ret)
		return ret;

	val = INV_ICM42607_ACCEL_CONFIG0_FS_SEL(conf->accel.fs) |
	INV_ICM42607_ACCEL_CONFIG0_ODR(conf->accel.odr);
	ret = regmap_write(st->map, INV_ICM42607_REG_ACCEL_CONFIG0, val);
	if (ret)
		return ret;

	val = INV_ICM42607_GYRO_CONFIG1_FILTER(conf->gyro.filter);
	ret = regmap_write(st->map, INV_ICM42607_REG_GYRO_CONFIG1, val);
	if (ret)
		return ret;

	val = INV_ICM42607_ACCEL_CONFIG1_FILTER(conf->accel.filter);
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
	msleep(INV_ICM42607_RESET_TIME_MS);

	ret = regmap_read(st->map, INV_ICM42607_REG_INT_STATUS, &val);
	if (ret)
		return ret;
	if (!(val & INV_ICM42607_INT_STATUS_RESET_DONE))
		return dev_err_probe(dev, -ENODEV,
				     "reset error, reset done bit not set\n");

	ret = bus_setup(st);
	if (ret)
		return ret;

	ret = regmap_update_bits(st->map, INV_ICM42607_REG_INTF_CONFIG0,
				 INV_ICM42607_INTF_CONFIG0_SENSOR_DATA_ENDIAN,
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

static int inv_icm42607_enable_vddio_reg(struct inv_icm42607_state *st)
{
	int ret;

	ret = regulator_enable(st->vddio_supply);
	if (ret)
		return ret;

	usleep_range(3000, 4000);

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

	if (chip < INV_CHIP_INVALID || chip >= INV_CHIP_NB)
		dev_warn_probe(dev, -ENODEV,
			       "Invalid chip = %d\n", chip);

	/* get INT1 only supported interrupt or fallback to first interrupt */
	irq = fwnode_irq_get_byname(fwnode, "INT1");
	if (irq < 0 && irq != -EPROBE_DEFER) {
		dev_info(dev, "no INT1 interrupt defined, fallback to first interrupt\n");
		irq = fwnode_irq_get(fwnode, 0);
	}
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
	mutex_init(&st->lock);
	st->chip = chip;
	st->map = regmap;
	st->irq = irq;

	ret = iio_read_mount_matrix(dev, &st->orientation);
	if (ret) {
		dev_err(dev, "failed to retrieve mounting matrix %d\n", ret);
		return ret;
	}

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get vdd regulator\n");

	msleep(INV_ICM42607_POWER_UP_TIME_MS);

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

	return ret;
}
EXPORT_SYMBOL_NS_GPL(inv_icm42607_core_probe, "IIO_ICM42607");

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-42607x device driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_INV_SENSORS_TIMESTAMP");
