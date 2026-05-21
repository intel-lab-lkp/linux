// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DWC I2C adapter driver (slave only).
 *
 * Based on the Synopsys DWC I2C adapter driver (master).
 *
 * Copyright (C) 2016 Synopsys Inc.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include "i2c-designware-core.h"

void i2c_dw_set_mode(struct dw_i2c_dev *dev, int mode)
{
	if (dev->mode == DW_IC_SLAVE && mode == DW_IC_SLAVE) {
		regmap_write(dev->map, DWC_IC_DAR, dev->slave->addr);
		__i2c_dw_enable(dev);
		dev->status = 0;
	}
}

static void i2c_dwc_configure_fifo_slave(struct dw_i2c_dev *dev)
{
	/* Configure the I2C slave. */
	regmap_write(dev->map, DWC_IC_CTRL, 0);
	regmap_write(dev->map, DWC_IC_CTRL, dev->slave_cfg);
	regmap_write(dev->map, DW_IC_INTR_MASK, DW_IC_INTR_SLAVE_MASK);
}

/**
 * i2c_dwc_init_slave() - Initialize the DWC i2c slave hardware
 * @dev: device private data
 *
 * This function configures and enables the I2C in slave mode.
 * This function is called during I2C init function, and in case of timeout at
 * run time.
 */
static int i2c_dwc_init_slave(struct dw_i2c_dev *dev)
{
	int ret;
	int r_value;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;

	/* Disable the adapter. */
	__i2c_dw_disable(dev);

	/* Write SDA hold time if supported */
	if (dev->sda_hold_time)
		regmap_write(dev->map, DW_IC_SDA_HOLD, dev->sda_hold_time);

	regmap_read(dev->map, DWC_IC_CAPABILITIES, &r_value);
	if (r_value & DWC_IC_CAPABILITIES_IC_SMBUS) {
		regmap_read(dev->map, DWC_IC_SMBUS_CAPABILITIES, &r_value);
		if (r_value & DWC_IC_SMBUS_CAPABILITIES_SMBUS_ARP) {
			regmap_write(dev->map, DW_IC_ENABLE, DWC_IC_ENABLE_DAR_EN);
			regmap_write(dev->map, DWC_IC_SMBUS_ARP_CTRL,
				     DWC_IC_SMBUS_ARP_CTRL_NARP_DEVICE_TYPE);
		}
	}

	i2c_dwc_configure_fifo_slave(dev);
	i2c_dw_release_lock(dev);

	return 0;
}

u32 i2c_dw_read_clear_intrbits_slave(struct dw_i2c_dev *dev)
{
	unsigned int stat;
	unsigned int smbus_stat;

	/*
	 * The IC_INTR_STAT register just indicates "enabled" interrupts.
	 * The unmasked raw version of interrupt status bits is available
	 * in the IC_RAW_INTR_STAT register.
	 *
	 * That is,
	 *   stat = readl(IC_INTR_STAT);
	 * equals to,
	 *   stat = readl(IC_RAW_INTR_STAT) & readl(IC_INTR_MASK);
	 *
	 * The raw version might be useful for debugging purposes.
	 */
	regmap_read(dev->map, DW_IC_INTR_STAT, &stat);
	regmap_read(dev->map, DWC_IC_SMBUS_INTR_STAT, &smbus_stat);

	/*
	 * Do not use the IC_CLR_INTR register to clear interrupts, or
	 * you'll miss some interrupts, triggered during the period from
	 * readl(IC_INTR_STAT) to readl(IC_CLR_INTR).
	 *
	 * Instead, use the separately-prepared IC_CLR_* registers.
	 */

	if (stat & DW_IC_INTR_TX_ABRT)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_TX_ABRT);
	if (stat & DW_IC_INTR_STOP_DET)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_STOP_DET);
	if (smbus_stat & DWC_R_SMBUS_ALERT_DET)
		regmap_write(dev->map, DWC_IC_SMBUS_INTR_CLR, DWC_CLR_SMBUS_ALERT_DET);

	i2c_dw_read_clear_intrbits_common(dev);

	return stat;
}

#if IS_ENABLED(CONFIG_I2C_DWC_SLAVE)
static int i2c_dwc_xfer_ms(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(adap);

	if (!dev->ms_adapter)
		return -ENXIO;

	return i2c_dw_xfer(dev->ms_adapter, msgs, num);
}
#endif

const struct i2c_algorithm i2c_dw_slave_algo = {
	.master_xfer = i2c_dwc_xfer_ms,
	.functionality = i2c_dw_func,
	.reg_slave = i2c_dw_reg_slave,
	.unreg_slave = i2c_dw_unreg_slave,
};

void i2c_dw_configure_slave(struct dw_i2c_dev *dev)
{
	dev->functionality = I2C_FUNC_SLAVE | DW_IC_DEFAULT_FUNCTIONALITY;

	dev->slave_cfg = DWC_IC_CTRL_RX_FIFO_FULL_HLD_CTRL |
			 DWC_IC_CTRL_STOP_DET_IFADDRESSED;

	dev->mode = DW_IC_SLAVE;
}

int i2c_dw_probe_slave(struct dw_i2c_dev *dev)
{
	struct i2c_adapter *adap = &dev->adapter;
	struct device_node *ms_node;
	int ret;

	ret = i2c_dwc_init_slave(dev);
	if (ret)
		return ret;

	snprintf(adap->name, sizeof(adap->name), "Synopsys DWC I2C Slave adapter");

	/* Get the specified I2C master adapter from DTS for MCTP */
	ms_node = of_parse_phandle(dev->dev->of_node, "starfive,mctp-i2c-ms", 0);
	if (ms_node) {
		struct platform_device *ms_pdev = of_find_device_by_node(ms_node);

		if (ms_pdev) {
			struct dw_i2c_dev *ms_dev = platform_get_drvdata(ms_pdev);

			if (ms_dev)
				dev->ms_adapter = &ms_dev->adapter;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(i2c_dw_probe_slave);

MODULE_AUTHOR("Sankarshan shukla <sashukla@synopsys.com>");
MODULE_DESCRIPTION("Synopsys DWC I2C bus slave adapter");
MODULE_LICENSE("GPL");
