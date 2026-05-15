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
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include "inv_icm42607.h"

static bool inv_icm42607_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case INV_ICM42607_REG_MCLK_RDY:
	case INV_ICM42607_REG_SIGNAL_PATH_RESET:
	case INV_ICM42607_REG_TEMP_DATA1 ... INV_ICM42607_REG_APEX_DATA5:
	case INV_ICM42607_REG_APEX_CONFIG0:
	case INV_ICM42607_REG_FIFO_CONFIG2 ... INV_ICM42607_REG_FIFO_CONFIG3:
	case INV_ICM42607_REG_FIFO_LOST_PKT0 ... INV_ICM42607_REG_APEX_DATA3:
	case INV_ICM42607_REG_INT_STATUS_DRDY:
	case INV_ICM42607_REG_INT_STATUS ... INV_ICM42607_REG_FIFO_DATA:
		return true;
	}

	return false;
}

const struct regmap_config inv_icm42607_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = INV_ICM42607_REG_WHOAMI,
	.cache_type = REGCACHE_MAPLE,
	.volatile_reg = inv_icm42607_is_volatile_reg,
};
EXPORT_SYMBOL_NS_GPL(inv_icm42607_regmap_config, "IIO_ICM42607");

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

const struct inv_icm42607_hw inv_icm42607_hw_data = {
	.whoami = INV_ICM42607_WHOAMI,
	.name = "icm42607",
	.conf = &inv_icm42607_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm42607_hw_data, "IIO_ICM42607");

const struct inv_icm42607_hw inv_icm42607p_hw_data = {
	.whoami = INV_ICM42607P_WHOAMI,
	.name = "icm42607p",
	.conf = &inv_icm42607_default_conf,
};
EXPORT_SYMBOL_NS_GPL(inv_icm42607p_hw_data, "IIO_ICM42607");

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
	const struct device *dev = regmap_get_device(st->map);
	unsigned int val;
	int ret;

	/*
	 * Setup the bus first in case we need to set the SPI mode or
	 * change the slew rate in order.
	 */
	ret = bus_setup(st);
	if (ret)
		return ret;

	ret = regmap_read(st->map, INV_ICM42607_REG_WHOAMI, &val);
	if (ret)
		return ret;

	if (val != st->hw->whoami)
		dev_warn(dev, "invalid whoami %#02x expected %#02x (%s)\n",
			 val, st->hw->whoami, st->hw->name);

	ret = regmap_write(st->map, INV_ICM42607_REG_SIGNAL_PATH_RESET,
			   INV_ICM42607_SIGNAL_PATH_RESET_SOFT_RESET);
	if (ret)
		return ret;

	/*
	 * In order to confirm a reset is complete, we need to read the reset
	 * bit, but in certain circumstances we need to set the bus up before
	 * we can do a read. So we should wait the required amount of time
	 * per the datasheet first, then set the bus up again, then read to
	 * ensure the reset status is done. Invalidate the regmap cache since
	 * we're doing a hardware reset.
	 */
	regcache_mark_dirty(st->map);

	fsleep(INV_ICM42607_RESET_TIME_MS * 1000);

	ret = bus_setup(st);
	if (ret)
		return ret;

	ret = regmap_read(st->map, INV_ICM42607_REG_INT_STATUS, &val);
	if (ret || (!(val & INV_ICM42607_INT_STATUS_RESET_DONE)))
		return dev_err_probe(dev, -EIO,
				     "reset error, reset done bit not set\n");

	ret = regmap_set_bits(st->map, INV_ICM42607_REG_INTF_CONFIG0,
			      INV_ICM42607_INTF_CONFIG0_SENSOR_DATA_ENDIAN);
	if (ret)
		return ret;

	ret = regmap_update_bits(st->map, INV_ICM42607_REG_INTF_CONFIG1,
				 INV_ICM42607_INTF_CONFIG1_CLKSEL_MASK,
				 INV_ICM42607_INTF_CONFIG1_CLKSEL_PLL);
	if (ret)
		return ret;

	return inv_icm42607_set_conf(st, st->hw->conf);
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

int inv_icm42607_core_probe(struct regmap *regmap, const struct inv_icm42607_hw *hw,
			    inv_icm42607_bus_setup bus_setup)
{
	struct device *dev = regmap_get_device(regmap);
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct inv_icm42607_state *st;
	int irq;
	int ret;

	irq = fwnode_irq_get_byname(fwnode, "INT1");
	if (!(irq > 0))
		return dev_err_probe(dev, -EINVAL, "Unable to get INT1 interrupt\n");

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	dev_set_drvdata(dev, st);

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	st->hw = hw;
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
		return dev_err_probe(dev, PTR_ERR(st->vddio_supply),
				     "Failed to get vddio regulator\n");

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

	return 0;
}
EXPORT_SYMBOL_NS_GPL(inv_icm42607_core_probe, "IIO_ICM42607");

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-42607x device driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_INV_SENSORS_TIMESTAMP");
