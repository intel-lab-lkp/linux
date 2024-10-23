/* SPDX-License-Identifier: GPL-2.0 */
//
// Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.org>
//
// Fuel gauge driver header for MAXIM 77705 charger/power-supply.

#ifndef __MAX77705_FUELGAUGE_H
#define __MAX77705_FUELGAUGE_H __FILE__

#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>

#define ALERT_EN			4
// Current and capacity values are displayed as a voltage
// and must be divided by the sense resistor to determine Amps or Amp-hours.
// This should be applied to all current, charge, energy registers,
// except ModelGauge m5 Algorithm related ones.
// current sense resolution
#define MAX77705_FG_CS_ADC_RESOLUTION	15625 // 1.5625 microvolts
// voltage sense resolution
#define MAX77705_FG_VS_ADC_RESOLUTION	78125 // 78.125 microvolts
// CONFIG_REG register
#define MAX77705_SOC_ALERT_EN_MASK	BIT(2)
// When set to 1, external temperature measurements should be written from the host
#define MAX77705_TEX_MASK		BIT(8)
// Enable Thermistor
#define MAX77705_ETHRM_MASK		BIT(5)
// CONFIG2_REG register
#define MAX77705_AUTO_DISCHARGE_EN_MASK BIT(9)
// STATUS_REG register
#define MAX77705_BAT_ABSENT_MASK	BIT(3)

// @brief Convert voltage register value to micro volts
// @param reg_val - register value
// @return voltage in micro Volts
inline u64 max77705_fg_vs_convert(u16 reg_val)
{
	return mult_frac(reg_val, MAX77705_FG_VS_ADC_RESOLUTION, 1000);
}

// @brief Convert current register value to micro volts
// @param reg_val - register value
// @param rsense_conductance - current sense resistor conductance in Ohm^-1
// @return voltage in micro Volts
inline s32 max77705_fg_cs_convert(s16 reg_val, u32 rsense_conductance)
{
	return mult_frac(reg_val * rsense_conductance, MAX77705_FG_CS_ADC_RESOLUTION,
			 10000);
}

struct max77705_fuelgauge_data {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy_fg;
	struct power_supply_battery_info *bat_info;
	u32 rsense_conductance;
};

#endif // __MAX77705_FUELGAUGE_H
