/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * max77705_fuelgauge.h
 * Samsung max77705 Fuel Gauge Header
 *
 * Copyright (C) 2015 Samsung Electronics, Inc.
 *
 * This software is 77854 under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __MAX77705_FUELGAUGE_H
#define __MAX77705_FUELGAUGE_H __FILE__

#include <linux/mfd/core.h>
#include <linux/mfd/max77705.h>
#include <linux/mfd/max77705-private.h>
#include <linux/regulator/machine.h>

#define ALERT_EN 0x04
#define CAPACITY_SCALE_DEFAULT_CURRENT 1000
#define CAPACITY_SCALE_HV_CURRENT 600
/*
 * Current and capacity values are displayed as a voltage
 * and must be divided by the sense resistor to determine Amps or Amp-hours.
 * This should be applied to all current, charge, energy registers,
 * except ModelGauge m5 Algorithm related ones.
 */
/* current sense resolution */
#define MAX77705_FG_CS_ADC_RESOLUTION 15625 /* 1.5625 microvolts */
/* voltage sense resolution */
#define MAX77705_FG_VS_ADC_RESOLUTION 78125 /* 78.125 microvolts */
/* CONFIG register */
#define MAX77705_SOC_ALERT_EN_MASK		BIT(2)
/* When set to 1, external temperature measurements should be written from the host */
#define MAX77705_TEX_MASK		BIT(8)
/* Enable Thermistor */
#define MAX77705_ETHRM_MASK		BIT(5)
/* CONFIG2 register */
#define MAX77705_AUTO_DISCHARGE_EN_MASK BIT(9)
/* DISCHARGE register*/
#define MAX77705_AUTO_DISCHARGE_VALUE_SHIFT 3
#define MAX77705_AUTO_DISCHARGE_VALUE_MASK 0xF8

#define MAX77705_FG_CS_MASK BIT(15)
/* MISCCFG register */
#define MAX77705_AUTO_DISCHARGE_VALUE_MASK 0xF8

/* adc resolution for voltage sensing is 78.125 microvolts */
inline u64 max77705_fg_vs_convert(u16 reg_val)
{
	u64 result = (u64)reg_val * MAX77705_FG_VS_ADC_RESOLUTION;

	return result / 1000;
}

/* adc resolution for current sensing is 1.5625 microvolts */
inline s32 max77705_fg_cs_convert(s16 reg_val, u32 rsense_conductance)
{
	s64 result = (s64)reg_val * rsense_conductance * MAX77705_FG_CS_ADC_RESOLUTION;

	return result / 10000;
}

struct max77705_fuelgauge_data {
	struct device           *dev;
	struct i2c_client       *i2c;
	struct i2c_client       *pmic;
	struct mutex            fuelgauge_mutex;
	struct max77705_dev	*max77705;
	struct max77705_platform_data *max77705_pdata;
	struct power_supply	      *psy_fg;
	struct delayed_work isr_work;

	int cable_type;
	bool is_charging;

	struct power_supply_battery_info *bat_info;

	struct mutex fg_lock;

	/* register programming */
	int reg_addr;
	u8 reg_data[2];

	unsigned int pre_soc;
	int fg_irq;

	int temperature;
	int low_temp_limit;

	bool auto_discharge_en;
	u32 discharge_temp_threshold;
	u32 discharge_volt_threshold;

	u32 rsense_conductance;
	u32 fuel_alert_soc;
};

#endif /* __MAX77705_FUELGAUGE_H */
