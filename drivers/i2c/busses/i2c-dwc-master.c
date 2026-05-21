// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Synopsys DWC I2C adapter driver (master only).
 *
 * Based on the TI DAVINCI I2C adapter driver.
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/units.h>

#include "i2c-designware-core.h"

#define AMD_TIMEOUT_MIN_US	25
#define AMD_TIMEOUT_MAX_US	250
#define AMD_MASTERCFG_MASK	GENMASK(15, 0)

/**
 * i2c_dwc_scl_hcnt() -  Calculate SCL HCNT
 * @ic_clk: Input clock in kHz
 * @thigh: Duration in ns of logic 1 to generate
 * @tr: SCL rise time in ns
 * @spk_cnt: Spike count
 */
static u32 i2c_dwc_scl_hcnt(u32 ic_clk, u32 thigh, u32 tr, u32 spk_cnt)
{
	u64 min_thigh_cnt, rise_cnt;

	/* Formula: cnt = f_kHz * t_ns * 10^(-6) */
	min_thigh_cnt = DIV_ROUND_CLOSEST_ULL((u64)ic_clk * thigh, MICRO);
	rise_cnt = DIV_ROUND_CLOSEST_ULL((u64)ic_clk * tr, MICRO);

	return max(5, min_thigh_cnt + rise_cnt - spk_cnt - 3);
}

/**
 * i2c_dwc_scl_lcnt() -  Calculate SCL LCNT
 * @ic_clk: Input clock in kHz
 * @tlow: Duration in ns of logic 0 to generate
 * @tf: SCL fall time in ns
 */
static u32 i2c_dwc_scl_lcnt(u32 ic_clk, u32 tlow, u32 tf)
{
	u64 min_tlow_cnt, fall_cnt;

	/* Formula: cnt = f_kHz * t_ns * 10^(-6) */
	min_tlow_cnt = DIV_ROUND_CLOSEST_ULL((u64)ic_clk * tlow, MICRO);
	fall_cnt = DIV_ROUND_CLOSEST_ULL((u64)ic_clk * tf, MICRO);

	return max(6, min_tlow_cnt + fall_cnt);
}

static void i2c_dwc_configure_fifo_master(struct dw_i2c_dev *dev)
{
	/* Configure Tx/Rx FIFO threshold levels */
	regmap_write(dev->map, DWC_IC_TX_TL, dev->tx_fifo_depth / 2);
	regmap_write(dev->map, DWC_IC_RX_TL, 0);

	/* Configure the I2C master */
	regmap_write(dev->map, DWC_IC_CTRL, dev->master_cfg);
}

static int i2c_dwc_set_timings_master(struct dw_i2c_dev *dev)
{
	u32 scl_falling_time = 0, scl_rising_time = 0;
	u32 scl_high_time = 0, scl_low_time = 0;
	struct i2c_timings *t = &dev->timings;
	unsigned int comp_param1;
	u32 ic_clk, spk_cnt;
	int ret;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;

	ret = regmap_read(dev->map, DWC_IC_CTRL, &comp_param1);
	i2c_dw_release_lock(dev);
	if (ret)
		return ret;

	ic_clk = i2c_dw_clk_rate(dev);

	/* 50ns maximum spike */
	spk_cnt = DIV_ROUND_CLOSEST_ULL((u64)ic_clk * 50, MICRO);

	regmap_write(dev->map, DWC_IC_HS_SPKLEN, spk_cnt);
	regmap_write(dev->map, DWC_IC_SPKLEN, spk_cnt);

	/* Parse user defined rise time and fall time*/
	if (t->scl_rise_ns)
		scl_rising_time = t->scl_rise_ns;

	if (t->scl_fall_ns)
		scl_falling_time = t->scl_fall_ns;

	/* Ensure the rise time and fall time should not lower than t_rise_max
	 * and t_fall_max specification, else it would run faster than expected
	 * frequency
	 */
	switch (t->bus_freq_hz) {
	case I2C_MAX_STANDARD_MODE_FREQ:
		scl_rising_time = max(scl_rising_time, 1000);
		scl_falling_time = max(scl_falling_time, 300);
		scl_high_time = 4000;	/* tHIGH_min = 4.0 us */
		scl_low_time = 4700;	/* tLOW_min = 4.7 us */
		break;
	case I2C_MAX_FAST_MODE_FREQ:
		scl_rising_time = max(scl_rising_time, 300);
		scl_falling_time = max(scl_falling_time, 300);
		scl_high_time = 600;	/* tHIGH_min = 600 ns */
		scl_low_time = 1300;	/* tLOW_min = 1.3 us */
		break;
	case I2C_MAX_FAST_MODE_PLUS_FREQ:
		scl_rising_time = max(scl_rising_time, 120);
		scl_falling_time = max(scl_falling_time, 120);
		scl_high_time = 260;	/* tHIGH_min = 260 ns */
		scl_low_time = 500;	/* tLOW_min = 500 ns */
		break;
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		scl_rising_time = max(scl_rising_time, 40);
		scl_falling_time = max(scl_falling_time, 40);
		scl_high_time = 60;	/* tHIGH_min = 60 ns */
		scl_low_time = 160;	/* tLOW_min = 160 ns */
		break;
	default:
		scl_rising_time = max(scl_rising_time, 1000);
		scl_falling_time = max(scl_falling_time, 300);
		scl_high_time = 4000;	/* tHIGH_min = 4.0 us */
		scl_low_time = 4700;	/* tLOW_min = 4.7 us */
		break;
	}

	ic_clk = i2c_dw_clk_rate(dev);

	if (!dev->scl_hcnt || !dev->scl_lcnt) {
		dev->scl_hcnt = i2c_dwc_scl_hcnt(ic_clk, scl_high_time,
						 scl_rising_time, spk_cnt);
		dev->scl_lcnt = i2c_dwc_scl_lcnt(ic_clk, scl_low_time,
						 scl_falling_time);
	}

	dev_dbg(dev->dev, "Bus speed: %s\n", i2c_freq_mode_string(t->bus_freq_hz));

	/* Check is high speed possible and fall back to fast mode if not */
	if ((dev->master_cfg & DWC_IC_CTRL_SPEED_MASK) == DWC_IC_CTRL_SPEED_HIGH) {
		if ((comp_param1 & DWC_IC_COMP_PARAM_1_SPEED_MODE_MASK)
			!= DWC_IC_COMP_PARAM_1_SPEED_MODE_HIGH) {
			dev_err(dev->dev, "High Speed not supported!\n");
			t->bus_freq_hz = I2C_MAX_FAST_MODE_FREQ;
			dev->master_cfg &= ~DWC_IC_CTRL_SPEED_MASK;
			dev->master_cfg |= DWC_IC_CTRL_SPEED_FAST;
			dev->hs_hcnt = 0;
			dev->hs_lcnt = 0;

			/* Replace with I2C_MAX_FAST_MODE_PLUS_FREQ */
			scl_rising_time = max(scl_rising_time, 120);
			scl_falling_time = max(scl_falling_time, 120);
			scl_high_time = 260;	/* tHIGH_min = 260 ns */
			scl_low_time = 500;	/* tLOW_min = 500 ns */

			dev->scl_hcnt = i2c_dwc_scl_hcnt(ic_clk, scl_high_time,
							 scl_rising_time, spk_cnt);
			dev->scl_lcnt = i2c_dwc_scl_lcnt(ic_clk, scl_low_time,
							 scl_falling_time);
		} else if (!dev->hs_hcnt || !dev->hs_lcnt) {
			dev->hs_hcnt = dev->scl_hcnt;
			dev->hs_lcnt = dev->scl_lcnt;
		}
		dev_dbg(dev->dev, "High Speed Mode HCNT:LCNT = %d:%d\n",
			dev->hs_hcnt, dev->hs_lcnt);
	} else {
		dev_dbg(dev->dev, "HCNT:LCNT: %d:%d\n", dev->scl_hcnt, dev->scl_lcnt);
	}

	return 0;
}

int i2c_dw_init(struct dw_i2c_dev *dev)
{
	int ret;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;

	/* Disable the adapter */
	__i2c_dw_disable(dev);

	/* Write standard speed timing parameters */
	regmap_write(dev->map, DWC_IC_SCL_HCNT, dev->scl_hcnt);
	regmap_write(dev->map, DWC_IC_SCL_LCNT, dev->scl_lcnt);

	/* Write high speed timing parameters if supported */
	if (dev->hs_hcnt && dev->hs_lcnt) {
		regmap_write(dev->map, DWC_IC_HS_SCL_HCNT, dev->hs_hcnt);
		regmap_write(dev->map, DWC_IC_HS_SCL_LCNT, dev->hs_lcnt);
	}

	/* Write SDA hold time if supported */
	if (dev->sda_hold_time)
		regmap_write(dev->map, DW_IC_SDA_HOLD, dev->sda_hold_time);

	i2c_dwc_configure_fifo_master(dev);
	i2c_dw_release_lock(dev);

	return 0;
}

static int i2c_dwc_set_sda_hold_time(struct dw_i2c_dev *dev)
{
	int ret = regmap_write_bits(dev->map,
			DW_IC_SDA_HOLD,
			DW_IC_SDA_HOLD_TX_MASK,
			dev->sda_hold_time);
	return ret;
}

void i2c_dw_xfer_init(struct dw_i2c_dev *dev)
{
	struct i2c_msg *msgs = dev->msgs;
	u32 ic_ctrl = 0, ic_tar = 0;
	unsigned int dummy;

	/* Disable the adapter */
	__i2c_dw_disable(dev);

	/* If the slave address is ten bit address, enable 10BITADDR */
	if (msgs[dev->msg_write_idx].flags & I2C_M_TEN) {
		ic_ctrl = DWC_IC_CTRL_10BITADDR_CTRLR;
		/*
		 * If I2C_DYNAMIC_TAR_UPDATE is set, the 10-bit addressing
		 * mode has to be enabled via bit 12 of IC_TAR register.
		 * We set it always as I2C_DYNAMIC_TAR_UPDATE can't be
		 * detected from registers.
		 */
		ic_tar = DW_IC_TAR_10BITADDR_MASTER;
	}

	/* From i2c-core-smbus.c, I2C_SMBUS_QUICK intentionally has msg[0].len = 0 */
	if (dev->msgs[0].len == 0)
		ic_tar |= DWC_IC_TAR_SMBUS_QUICK_CMD | DWC_IC_TAR_SPECIAL;

	regmap_update_bits(dev->map, DWC_IC_CTRL, DWC_IC_CTRL_10BITADDR_CTRLR,
			   ic_ctrl);

	/*
	 * Set the slave (target) address and enable 10-bit addressing mode
	 * if applicable.
	 */
	regmap_write(dev->map, DW_IC_TAR,
		     msgs[dev->msg_write_idx].addr | ic_tar);

	/* Enforce disabled interrupts (due to HW issues) */
	regmap_write(dev->map, DW_IC_INTR_MASK, 0);

	/* Enable the adapter */
	__i2c_dw_enable(dev);

	/* Dummy read to avoid the register getting stuck on Bay Trail */
	regmap_read(dev->map, DW_IC_ENABLE_STATUS, &dummy);

	/* Clear and enable interrupts */
	regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_CLR_INTR);
	regmap_write(dev->map, DW_IC_INTR_MASK, DW_IC_INTR_MASTER_MASK);
}

void i2c_dw_read_clear_intrbits_common(struct dw_i2c_dev *dev)
{
	unsigned int stat;

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
	/*
	 * Do not use the IC_CLR_INTR register to clear interrupts, or
	 * you'll miss some interrupts, triggered during the period from
	 * readl(IC_INTR_STAT) to readl(IC_CLR_INTR).
	 *
	 * Instead, use the separately-prepared IC_CLR_* registers.
	 */
	if (stat & DW_IC_INTR_RX_UNDER)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_RX_UNDER);
	if (stat & DW_IC_INTR_RX_OVER)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_RX_OVER);
	if (stat & DW_IC_INTR_TX_OVER)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_TX_OVER);
	if (stat & DW_IC_INTR_RD_REQ)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_RD_REQ);
	if (stat & DW_IC_INTR_RX_DONE)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_RX_DONE);
	if (stat & DW_IC_INTR_ACTIVITY)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_ACTIVITY);
	if (stat & DW_IC_INTR_START_DET)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_START_DET);
	if (stat & DW_IC_INTR_GEN_CALL)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_GEN_CALL);
}
EXPORT_SYMBOL_GPL(i2c_dw_read_clear_intrbits_common);

u32 i2c_dw_read_clear_intrbits(struct dw_i2c_dev *dev)
{
	unsigned int stat;

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

	if (stat & DW_IC_INTR_TX_ABRT) {
		/*
		 * The IC_TX_ABRT_SOURCE register is cleared whenever
		 * the IC_CLR_TX_ABRT is read.  Preserve it beforehand.
		 */
		regmap_read(dev->map, DWC_IC_TX_TRMNT_SOURCE, &dev->abort_source);
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_TX_ABRT);
	}
	if ((stat & DW_IC_INTR_STOP_DET) &&
	    (dev->rx_outstanding == 0 || (stat & DW_IC_INTR_RX_FULL)))
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_STOP_DET);
	if (stat & DW_IC_INTR_RESTART_DET)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_RESTART_DET);
	if (stat & DWC_IC_INTR_SCL_STUCK_AT_LOW)
		regmap_write(dev->map, DWC_IC_INTR_CLR, DWC_IC_CLR_SCL_STUCK_DET);

	i2c_dw_read_clear_intrbits_common(dev);

	return stat;
}

void i2c_dw_configure_master(struct dw_i2c_dev *dev)
{
	struct i2c_timings *t = &dev->timings;

	dev->functionality = I2C_FUNC_10BIT_ADDR | DW_IC_DEFAULT_FUNCTIONALITY |
		I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_PEC;

	dev->master_cfg = DWC_IC_CTRL_OP_MODE;

	dev->mode = DW_IC_MASTER;

	switch (t->bus_freq_hz) {
	case I2C_MAX_STANDARD_MODE_FREQ:
		dev->master_cfg |= DWC_IC_CTRL_SPEED_STD;
		break;
	case I2C_MAX_FAST_MODE_FREQ:
		dev->master_cfg |= DWC_IC_CTRL_SPEED_FAST;
		break;
	case I2C_MAX_FAST_MODE_PLUS_FREQ:
		dev->master_cfg |= DWC_IC_CTRL_SPEED_FAST;
		break;
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		dev->master_cfg |= DWC_IC_CTRL_SPEED_HIGH;
		break;
	default:
		dev->master_cfg |= DWC_IC_CTRL_SPEED_FAST;
	}

	dev->set_sda_hold_time = i2c_dwc_set_sda_hold_time;
}

int i2c_dw_probe_master(struct dw_i2c_dev *dev)
{
	struct i2c_adapter *adap = &dev->adapter;
	unsigned int ic_ctrl;
	int ret;

	init_completion(&dev->cmd_complete);

	ret = i2c_dwc_set_timings_master(dev);
	if (ret)
		return ret;

	/* Lock the bus for accessing DWC_IC_CTRL */
	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;

	/*
	 * On AMD platforms BIOS advertises the bus clear feature
	 * and enables the SCL/SDA stuck low. SMU FW does the
	 * bus recovery process. Driver should not ignore this BIOS
	 * advertisement of bus clear feature.
	 */
	ret = regmap_read(dev->map, DWC_IC_CTRL, &ic_ctrl);
	i2c_dw_release_lock(dev);
	if (ret)
		return ret;

	if (ic_ctrl & DWC_IC_CTRL_BUS_CLEAR_CTRL)
		dev->master_cfg |= DWC_IC_CTRL_BUS_CLEAR_CTRL;

	ret = i2c_dw_init(dev);
	if (ret)
		return ret;

	snprintf(adap->name, sizeof(adap->name),
		 "Synopsys DWC I2C adapter");

	ret = i2c_dw_init_recovery_info(dev);
	if (ret)
		return ret;

	return ret;
}

MODULE_AUTHOR("Sankarshan shukla <sashukla@synopsys.com>");
MODULE_DESCRIPTION("Synopsys DWC I2C bus master adapter");
MODULE_LICENSE("GPL");
