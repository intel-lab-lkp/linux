// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2016, Fuzhou Rockchip Electronics Co., Ltd
 * Caesar Wang <wxt@rock-chips.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/thermal.h>
#include <linux/mfd/syscon.h>
#include <linux/pinctrl/consumer.h>

#include "rockchip_thermal_tables.h"
#include "thermal_hwmon.h"

/*
 * If the temperature over a period of time High,
 * the resulting TSHUT gave CRU module,let it reset the entire chip,
 * or via GPIO give PMIC.
 */
enum tshut_mode {
	TSHUT_MODE_CRU = 0,
	TSHUT_MODE_GPIO,
};

/*
 * The system Temperature Sensors tshut(tshut) polarity
 * the bit 8 is tshut polarity.
 * 0: low active, 1: high active
 */
enum tshut_polarity {
	TSHUT_LOW_ACTIVE = 0,
	TSHUT_HIGH_ACTIVE,
};

/*
 * The conversion table has the adc value and temperature.
 * ADC_DECREMENT: the adc value is of diminishing.(e.g. rk3288_code_table)
 * ADC_INCREMENT: the adc value is incremental.(e.g. rk3368_code_table)
 */
enum adc_sort_mode {
	ADC_DECREMENT = 0,
	ADC_INCREMENT,
};

/**
 * struct chip_tsadc_table - hold information about chip-specific differences
 * @id: conversion table
 * @length: size of conversion table
 * @data_mask: mask to apply on data inputs
 * @mode: sort mode of this adc variant (incrementing or decrementing)
 */
struct chip_tsadc_table {
	const struct tsadc_table *id;
	unsigned int length;
	u32 data_mask;
	enum adc_sort_mode mode;
};

/**
 * struct rockchip_tsadc_chip - hold the private data of tsadc chip
 * @chn_offset: the channel offset of the first channel
 * @chn_num: the channel number of tsadc chip
 * @tshut_temp: the hardware-controlled shutdown temperature value
 * @tshut_mode: the hardware-controlled shutdown mode (0:CRU 1:GPIO)
 * @tshut_polarity: the hardware-controlled active polarity (0:LOW 1:HIGH)
 * @initialize: SoC special initialize tsadc controller method
 * @irq_ack: clear the interrupt
 * @control: enable/disable method for the tsadc controller
 * @get_temp: get the temperature
 * @set_alarm_temp: set the high temperature interrupt
 * @set_tshut_temp: set the hardware-controlled shutdown temperature
 * @set_tshut_mode: set the hardware-controlled shutdown mode
 * @table: the chip-specific conversion table
 */
struct rockchip_tsadc_chip {
	/* The sensor id of chip correspond to the ADC channel */
	int chn_offset;
	int chn_num;

	/* The hardware-controlled tshut property */
	int tshut_temp;
	enum tshut_mode tshut_mode;
	enum tshut_polarity tshut_polarity;

	/* Chip-wide methods */
	void (*initialize)(struct regmap *grf,
			   void __iomem *reg, enum tshut_polarity p);
	void (*irq_ack)(void __iomem *reg);
	void (*control)(void __iomem *reg, bool on);

	/* Per-sensor methods */
	int (*get_temp)(const struct chip_tsadc_table *table,
			int chn, void __iomem *reg, int *temp);
	int (*set_alarm_temp)(const struct chip_tsadc_table *table,
			      int chn, void __iomem *reg, int temp);
	int (*set_tshut_temp)(const struct chip_tsadc_table *table,
			      int chn, void __iomem *reg, int temp);
	void (*set_tshut_mode)(int chn, void __iomem *reg, enum tshut_mode m);

	/* Per-table methods */
	struct chip_tsadc_table table;
};

/**
 * struct rockchip_thermal_sensor - hold the information of thermal sensor
 * @thermal:  pointer to the platform/configuration data
 * @tzd: pointer to a thermal zone
 * @id: identifier of the thermal sensor
 */
struct rockchip_thermal_sensor {
	struct rockchip_thermal_data *thermal;
	struct thermal_zone_device *tzd;
	int id;
};

/**
 * struct rockchip_thermal_data - hold the private data of thermal driver
 * @chip: pointer to the platform/configuration data
 * @pdev: platform device of thermal
 * @reset: the reset controller of tsadc
 * @sensors: array of thermal sensors
 * @clk: the controller clock is divided by the exteral 24MHz
 * @pclk: the advanced peripherals bus clock
 * @grf: the general register file will be used to do static set by software
 * @regs: the base address of tsadc controller
 * @tshut_temp: the hardware-controlled shutdown temperature value
 * @tshut_mode: the hardware-controlled shutdown mode (0:CRU 1:GPIO)
 * @tshut_polarity: the hardware-controlled active polarity (0:LOW 1:HIGH)
 */
struct rockchip_thermal_data {
	const struct rockchip_tsadc_chip *chip;
	struct platform_device *pdev;
	struct reset_control *reset;

	struct rockchip_thermal_sensor *sensors;

	struct clk *clk;
	struct clk *pclk;

	struct regmap *grf;
	void __iomem *regs;

	int tshut_temp;
	enum tshut_mode tshut_mode;
	enum tshut_polarity tshut_polarity;
};

/*
 * TSADC Sensor Register description:
 *
 * TSADCV2_* are used for RK3288 SoCs, the other chips can reuse it.
 * TSADCV3_* are used for newer SoCs than RK3288. (e.g: RK3228, RK3399)
 *
 */
#define TSADCV2_USER_CON			0x00
#define TSADCV2_AUTO_CON			0x04
#define TSADCV2_INT_EN				0x08
#define TSADCV2_INT_PD				0x0c
#define TSADCV3_AUTO_SRC_CON			0x0c
#define TSADCV3_HT_INT_EN			0x14
#define TSADCV3_HSHUT_GPIO_INT_EN		0x18
#define TSADCV3_HSHUT_CRU_INT_EN		0x1c
#define TSADCV3_INT_PD				0x24
#define TSADCV3_HSHUT_PD			0x28
#define TSADCV2_DATA(chn)			(0x20 + (chn) * 0x04)
#define TSADCV2_COMP_INT(chn)		        (0x30 + (chn) * 0x04)
#define TSADCV2_COMP_SHUT(chn)		        (0x40 + (chn) * 0x04)
#define TSADCV3_DATA(chn)			(0x2c + (chn) * 0x04)
#define TSADCV3_COMP_INT(chn)		        (0x6c + (chn) * 0x04)
#define TSADCV3_COMP_SHUT(chn)		        (0x10c + (chn) * 0x04)
#define TSADCV2_HIGHT_INT_DEBOUNCE		0x60
#define TSADCV2_HIGHT_TSHUT_DEBOUNCE		0x64
#define TSADCV3_HIGHT_INT_DEBOUNCE		0x14c
#define TSADCV3_HIGHT_TSHUT_DEBOUNCE		0x150
#define TSADCV2_AUTO_PERIOD			0x68
#define TSADCV2_AUTO_PERIOD_HT			0x6c
#define TSADCV3_AUTO_PERIOD			0x154
#define TSADCV3_AUTO_PERIOD_HT			0x158

#define TSADCV2_AUTO_EN				BIT(0)
#define TSADCV2_AUTO_EN_MASK			BIT(16)
#define TSADCV2_AUTO_SRC_EN(chn)		BIT(4 + (chn))
#define TSADCV3_AUTO_SRC_EN(chn)		BIT(chn)
#define TSADCV3_AUTO_SRC_EN_MASK(chn)		BIT(16 + chn)
#define TSADCV2_AUTO_TSHUT_POLARITY_HIGH	BIT(8)
#define TSADCV2_AUTO_TSHUT_POLARITY_MASK	BIT(24)

#define TSADCV3_AUTO_Q_SEL_EN			BIT(1)

#define TSADCV2_INT_SRC_EN(chn)			BIT(chn)
#define TSADCV2_INT_SRC_EN_MASK(chn)		BIT(16 + (chn))
#define TSADCV2_SHUT_2GPIO_SRC_EN(chn)		BIT(4 + (chn))
#define TSADCV2_SHUT_2CRU_SRC_EN(chn)		BIT(8 + (chn))

#define TSADCV2_INT_PD_CLEAR_MASK		~BIT(8)
#define TSADCV3_INT_PD_CLEAR_MASK		~BIT(16)
#define TSADCV4_INT_PD_CLEAR_MASK		0xffffffff

#define TSADCV2_HIGHT_INT_DEBOUNCE_COUNT	4
#define TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT	4
#define TSADCV2_AUTO_PERIOD_TIME		250 /* 250ms */
#define TSADCV2_AUTO_PERIOD_HT_TIME		50  /* 50ms */
#define TSADCV3_AUTO_PERIOD_TIME		1875 /* 2.5ms */
#define TSADCV3_AUTO_PERIOD_HT_TIME		1875 /* 2.5ms */

#define TSADCV5_AUTO_PERIOD_TIME		1622 /* 2.5ms */
#define TSADCV5_AUTO_PERIOD_HT_TIME		1622 /* 2.5ms */
#define TSADCV6_AUTO_PERIOD_TIME		5000 /* 2.5ms */
#define TSADCV6_AUTO_PERIOD_HT_TIME		5000 /* 2.5ms */

#define TSADCV2_USER_INTER_PD_SOC		0x340 /* 13 clocks */
#define TSADCV5_USER_INTER_PD_SOC		0xfc0 /* 97us, at least 90us */

#define GRF_SARADC_TESTBIT			0x0e644
#define GRF_TSADC_TESTBIT_L			0x0e648
#define GRF_TSADC_TESTBIT_H			0x0e64c

#define PX30_GRF_SOC_CON2			0x0408

#define RK3568_GRF_TSADC_CON			0x0600
#define RK3568_GRF_TSADC_ANA_REG0		(0x10001 << 0)
#define RK3568_GRF_TSADC_ANA_REG1		(0x10001 << 1)
#define RK3568_GRF_TSADC_ANA_REG2		(0x10001 << 2)
#define RK3568_GRF_TSADC_TSEN			(0x10001 << 8)

#define RK3588_GRF0_TSADC_CON			0x0100

#define RK3588_GRF0_TSADC_TRM			(0xff0077 << 0)
#define RK3588_GRF0_TSADC_SHUT_2CRU		(0x30003 << 10)
#define RK3588_GRF0_TSADC_SHUT_2GPIO		(0x70007 << 12)

#define GRF_SARADC_TESTBIT_ON			(0x10001 << 2)
#define GRF_TSADC_TESTBIT_H_ON			(0x10001 << 2)
#define GRF_TSADC_VCM_EN_L			(0x10001 << 7)
#define GRF_TSADC_VCM_EN_H			(0x10001 << 7)

#define GRF_CON_TSADC_CH_INV			(0x10001 << 1)

static u32 rk_tsadcv2_temp_to_code(const struct chip_tsadc_table *table,
				   int temp)
{
	int high, low, mid;
	unsigned long num;
	unsigned int denom;
	u32 error = table->data_mask;

	low = 0;
	high = (table->length - 1) - 1; /* ignore the last check for table */
	mid = (high + low) / 2;

	/* Return mask code data when the temp is over table range */
	if (temp < table->id[low].temp || temp > table->id[high].temp)
		goto exit;

	while (low <= high) {
		if (temp == table->id[mid].temp)
			return table->id[mid].code;
		else if (temp < table->id[mid].temp)
			high = mid - 1;
		else
			low = mid + 1;
		mid = (low + high) / 2;
	}

	/*
	 * The conversion code granularity provided by the table. Let's
	 * assume that the relationship between temperature and
	 * analog value between 2 table entries is linear and interpolate
	 * to produce less granular result.
	 */
	num = abs(table->id[mid + 1].code - table->id[mid].code);
	num *= temp - table->id[mid].temp;
	denom = table->id[mid + 1].temp - table->id[mid].temp;

	switch (table->mode) {
	case ADC_DECREMENT:
		return table->id[mid].code - (num / denom);
	case ADC_INCREMENT:
		return table->id[mid].code + (num / denom);
	default:
		pr_err("%s: unknown table mode: %d\n", __func__, table->mode);
		return error;
	}

exit:
	pr_err("%s: invalid temperature, temp=%d error=%d\n",
	       __func__, temp, error);
	return error;
}

static int rk_tsadcv2_code_to_temp(const struct chip_tsadc_table *table,
				   u32 code, int *temp)
{
	unsigned int low = 1;
	unsigned int high = table->length - 1;
	unsigned int mid = (low + high) / 2;
	unsigned int num;
	unsigned long denom;

	WARN_ON(table->length < 2);

	switch (table->mode) {
	case ADC_DECREMENT:
		code &= table->data_mask;
		if (code <= table->id[high].code)
			return -EAGAIN;		/* Incorrect reading */

		while (low <= high) {
			if (code >= table->id[mid].code &&
			    code < table->id[mid - 1].code)
				break;
			else if (code < table->id[mid].code)
				low = mid + 1;
			else
				high = mid - 1;

			mid = (low + high) / 2;
		}
		break;
	case ADC_INCREMENT:
		code &= table->data_mask;
		if (code < table->id[low].code)
			return -EAGAIN;		/* Incorrect reading */

		while (low <= high) {
			if (code <= table->id[mid].code &&
			    code > table->id[mid - 1].code)
				break;
			else if (code > table->id[mid].code)
				low = mid + 1;
			else
				high = mid - 1;

			mid = (low + high) / 2;
		}
		break;
	default:
		pr_err("%s: unknown table mode: %d\n", __func__, table->mode);
		return -EINVAL;
	}

	/*
	 * The 5C granularity provided by the table is too much. Let's
	 * assume that the relationship between sensor readings and
	 * temperature between 2 table entries is linear and interpolate
	 * to produce less granular result.
	 */
	num = table->id[mid].temp - table->id[mid - 1].temp;
	num *= abs(table->id[mid - 1].code - code);
	denom = abs(table->id[mid - 1].code - table->id[mid].code);
	*temp = table->id[mid - 1].temp + (num / denom);

	return 0;
}

/**
 * rk_tsadcv2_initialize - initialize TASDC Controller.
 * @grf: the general register file will be used to do static set by software
 * @regs: the base address of tsadc controller
 * @tshut_polarity: the hardware-controlled active polarity (0:LOW 1:HIGH)
 *
 * (1) Set TSADC_V2_AUTO_PERIOD:
 *     Configure the interleave between every two accessing of
 *     TSADC in normal operation.
 *
 * (2) Set TSADCV2_AUTO_PERIOD_HT:
 *     Configure the interleave between every two accessing of
 *     TSADC after the temperature is higher than COM_SHUT or COM_INT.
 *
 * (3) Set TSADCV2_HIGH_INT_DEBOUNCE and TSADC_HIGHT_TSHUT_DEBOUNCE:
 *     If the temperature is higher than COMP_INT or COMP_SHUT for
 *     "debounce" times, TSADC controller will generate interrupt or TSHUT.
 */
static void rk_tsadcv2_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	if (tshut_polarity == TSHUT_HIGH_ACTIVE)
		writel_relaxed(0U | TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);
	else
		writel_relaxed(0U & ~TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);

	writel_relaxed(TSADCV2_AUTO_PERIOD_TIME, regs + TSADCV2_AUTO_PERIOD);
	writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
		       regs + TSADCV2_HIGHT_INT_DEBOUNCE);
	writel_relaxed(TSADCV2_AUTO_PERIOD_HT_TIME,
		       regs + TSADCV2_AUTO_PERIOD_HT);
	writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
		       regs + TSADCV2_HIGHT_TSHUT_DEBOUNCE);
}

/**
 * rk_tsadcv3_initialize - initialize TASDC Controller.
 * @grf: the general register file will be used to do static set by software
 * @regs: the base address of tsadc controller
 * @tshut_polarity: the hardware-controlled active polarity (0:LOW 1:HIGH)
 *
 * (1) The tsadc control power sequence.
 *
 * (2) Set TSADC_V2_AUTO_PERIOD:
 *     Configure the interleave between every two accessing of
 *     TSADC in normal operation.
 *
 * (2) Set TSADCV2_AUTO_PERIOD_HT:
 *     Configure the interleave between every two accessing of
 *     TSADC after the temperature is higher than COM_SHUT or COM_INT.
 *
 * (3) Set TSADCV2_HIGH_INT_DEBOUNCE and TSADC_HIGHT_TSHUT_DEBOUNCE:
 *     If the temperature is higher than COMP_INT or COMP_SHUT for
 *     "debounce" times, TSADC controller will generate interrupt or TSHUT.
 */
static void rk_tsadcv3_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	/* The tsadc control power sequence */
	if (IS_ERR(grf)) {
		/* Set interleave value to workround ic time sync issue */
		writel_relaxed(TSADCV2_USER_INTER_PD_SOC, regs +
			       TSADCV2_USER_CON);

		writel_relaxed(TSADCV2_AUTO_PERIOD_TIME,
			       regs + TSADCV2_AUTO_PERIOD);
		writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
			       regs + TSADCV2_HIGHT_INT_DEBOUNCE);
		writel_relaxed(TSADCV2_AUTO_PERIOD_HT_TIME,
			       regs + TSADCV2_AUTO_PERIOD_HT);
		writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
			       regs + TSADCV2_HIGHT_TSHUT_DEBOUNCE);

	} else {
		/* Enable the voltage common mode feature */
		regmap_write(grf, GRF_TSADC_TESTBIT_L, GRF_TSADC_VCM_EN_L);
		regmap_write(grf, GRF_TSADC_TESTBIT_H, GRF_TSADC_VCM_EN_H);

		usleep_range(15, 100); /* The spec note says at least 15 us */
		regmap_write(grf, GRF_SARADC_TESTBIT, GRF_SARADC_TESTBIT_ON);
		regmap_write(grf, GRF_TSADC_TESTBIT_H, GRF_TSADC_TESTBIT_H_ON);
		usleep_range(90, 200); /* The spec note says at least 90 us */

		writel_relaxed(TSADCV3_AUTO_PERIOD_TIME,
			       regs + TSADCV2_AUTO_PERIOD);
		writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
			       regs + TSADCV2_HIGHT_INT_DEBOUNCE);
		writel_relaxed(TSADCV3_AUTO_PERIOD_HT_TIME,
			       regs + TSADCV2_AUTO_PERIOD_HT);
		writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
			       regs + TSADCV2_HIGHT_TSHUT_DEBOUNCE);
	}

	if (tshut_polarity == TSHUT_HIGH_ACTIVE)
		writel_relaxed(0U | TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);
	else
		writel_relaxed(0U & ~TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);
}

static void rk_tsadcv4_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	rk_tsadcv2_initialize(grf, regs, tshut_polarity);
	regmap_write(grf, PX30_GRF_SOC_CON2, GRF_CON_TSADC_CH_INV);
}

static void rk_tsadcv7_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	writel_relaxed(TSADCV5_USER_INTER_PD_SOC, regs + TSADCV2_USER_CON);
	writel_relaxed(TSADCV5_AUTO_PERIOD_TIME, regs + TSADCV2_AUTO_PERIOD);
	writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
		       regs + TSADCV2_HIGHT_INT_DEBOUNCE);
	writel_relaxed(TSADCV5_AUTO_PERIOD_HT_TIME,
		       regs + TSADCV2_AUTO_PERIOD_HT);
	writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
		       regs + TSADCV2_HIGHT_TSHUT_DEBOUNCE);

	if (tshut_polarity == TSHUT_HIGH_ACTIVE)
		writel_relaxed(0U | TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);
	else
		writel_relaxed(0U & ~TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);

	/*
	 * The general register file will is optional
	 * and might not be available.
	 */
	if (!IS_ERR(grf)) {
		regmap_write(grf, RK3568_GRF_TSADC_CON, RK3568_GRF_TSADC_TSEN);
		/*
		 * RK3568 TRM, section 18.5. requires a delay no less
		 * than 10us between the rising edge of tsadc_tsen_en
		 * and the rising edge of tsadc_ana_reg_0/1/2.
		 */
		udelay(15);
		regmap_write(grf, RK3568_GRF_TSADC_CON, RK3568_GRF_TSADC_ANA_REG0);
		regmap_write(grf, RK3568_GRF_TSADC_CON, RK3568_GRF_TSADC_ANA_REG1);
		regmap_write(grf, RK3568_GRF_TSADC_CON, RK3568_GRF_TSADC_ANA_REG2);

		/*
		 * RK3568 TRM, section 18.5. requires a delay no less
		 * than 90us after the rising edge of tsadc_ana_reg_0/1/2.
		 */
		usleep_range(100, 200);
	}
}

static void rk_tsadcv8_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	writel_relaxed(TSADCV6_AUTO_PERIOD_TIME, regs + TSADCV3_AUTO_PERIOD);
	writel_relaxed(TSADCV6_AUTO_PERIOD_HT_TIME,
		       regs + TSADCV3_AUTO_PERIOD_HT);
	writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
		       regs + TSADCV3_HIGHT_INT_DEBOUNCE);
	writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
		       regs + TSADCV3_HIGHT_TSHUT_DEBOUNCE);
	if (tshut_polarity == TSHUT_HIGH_ACTIVE)
		writel_relaxed(TSADCV2_AUTO_TSHUT_POLARITY_HIGH |
			       TSADCV2_AUTO_TSHUT_POLARITY_MASK,
			       regs + TSADCV2_AUTO_CON);
	else
		writel_relaxed(TSADCV2_AUTO_TSHUT_POLARITY_MASK,
			       regs + TSADCV2_AUTO_CON);
}

static void rk_tsadcv2_irq_ack(void __iomem *regs)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_INT_PD);
	writel_relaxed(val & TSADCV2_INT_PD_CLEAR_MASK, regs + TSADCV2_INT_PD);
}

static void rk_tsadcv3_irq_ack(void __iomem *regs)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_INT_PD);
	writel_relaxed(val & TSADCV3_INT_PD_CLEAR_MASK, regs + TSADCV2_INT_PD);
}

static void rk_tsadcv4_irq_ack(void __iomem *regs)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV3_INT_PD);
	writel_relaxed(val & TSADCV4_INT_PD_CLEAR_MASK, regs + TSADCV3_INT_PD);
	val = readl_relaxed(regs + TSADCV3_HSHUT_PD);
	writel_relaxed(val & TSADCV3_INT_PD_CLEAR_MASK,
		       regs + TSADCV3_HSHUT_PD);
}

static void rk_tsadcv2_control(void __iomem *regs, bool enable)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_AUTO_CON);
	if (enable)
		val |= TSADCV2_AUTO_EN;
	else
		val &= ~TSADCV2_AUTO_EN;

	writel_relaxed(val, regs + TSADCV2_AUTO_CON);
}

/**
 * rk_tsadcv3_control - the tsadc controller is enabled or disabled.
 * @regs: the base address of tsadc controller
 * @enable: boolean flag to enable the controller
 *
 * NOTE: TSADC controller works at auto mode, and some SoCs need set the
 * tsadc_q_sel bit on TSADCV2_AUTO_CON[1]. The (1024 - tsadc_q) as output
 * adc value if setting this bit to enable.
 */
static void rk_tsadcv3_control(void __iomem *regs, bool enable)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_AUTO_CON);
	if (enable)
		val |= TSADCV2_AUTO_EN | TSADCV3_AUTO_Q_SEL_EN;
	else
		val &= ~TSADCV2_AUTO_EN;

	writel_relaxed(val, regs + TSADCV2_AUTO_CON);
}

static void rk_tsadcv4_control(void __iomem *regs, bool enable)
{
	u32 val;

	if (enable)
		val = TSADCV2_AUTO_EN | TSADCV2_AUTO_EN_MASK;
	else
		val = TSADCV2_AUTO_EN_MASK;

	writel_relaxed(val, regs + TSADCV2_AUTO_CON);
}

static int rk_tsadcv2_get_temp(const struct chip_tsadc_table *table,
			       int chn, void __iomem *regs, int *temp)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_DATA(chn));

	return rk_tsadcv2_code_to_temp(table, val, temp);
}

static int rk_tsadcv4_get_temp(const struct chip_tsadc_table *table,
			       int chn, void __iomem *regs, int *temp)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV3_DATA(chn));

	return rk_tsadcv2_code_to_temp(table, val, temp);
}

static int rk_tsadcv2_alarm_temp(const struct chip_tsadc_table *table,
				 int chn, void __iomem *regs, int temp)
{
	u32 alarm_value;
	u32 int_en, int_clr;

	/*
	 * In some cases, some sensors didn't need the trip points, the
	 * set_trips will pass {-INT_MAX, INT_MAX} to trigger tsadc alarm
	 * in the end, ignore this case and disable the high temperature
	 * interrupt.
	 */
	if (temp == INT_MAX) {
		int_clr = readl_relaxed(regs + TSADCV2_INT_EN);
		int_clr &= ~TSADCV2_INT_SRC_EN(chn);
		writel_relaxed(int_clr, regs + TSADCV2_INT_EN);
		return 0;
	}

	/* Make sure the value is valid */
	alarm_value = rk_tsadcv2_temp_to_code(table, temp);
	if (alarm_value == table->data_mask)
		return -ERANGE;

	writel_relaxed(alarm_value & table->data_mask,
		       regs + TSADCV2_COMP_INT(chn));

	int_en = readl_relaxed(regs + TSADCV2_INT_EN);
	int_en |= TSADCV2_INT_SRC_EN(chn);
	writel_relaxed(int_en, regs + TSADCV2_INT_EN);

	return 0;
}

static int rk_tsadcv3_alarm_temp(const struct chip_tsadc_table *table,
				 int chn, void __iomem *regs, int temp)
{
	u32 alarm_value;

	/*
	 * In some cases, some sensors didn't need the trip points, the
	 * set_trips will pass {-INT_MAX, INT_MAX} to trigger tsadc alarm
	 * in the end, ignore this case and disable the high temperature
	 * interrupt.
	 */
	if (temp == INT_MAX) {
		writel_relaxed(TSADCV2_INT_SRC_EN_MASK(chn),
			       regs + TSADCV3_HT_INT_EN);
		return 0;
	}
	/* Make sure the value is valid */
	alarm_value = rk_tsadcv2_temp_to_code(table, temp);
	if (alarm_value == table->data_mask)
		return -ERANGE;
	writel_relaxed(alarm_value & table->data_mask,
		       regs + TSADCV3_COMP_INT(chn));
	writel_relaxed(TSADCV2_INT_SRC_EN(chn) | TSADCV2_INT_SRC_EN_MASK(chn),
		       regs + TSADCV3_HT_INT_EN);
	return 0;
}

static int rk_tsadcv2_tshut_temp(const struct chip_tsadc_table *table,
				 int chn, void __iomem *regs, int temp)
{
	u32 tshut_value, val;

	/* Make sure the value is valid */
	tshut_value = rk_tsadcv2_temp_to_code(table, temp);
	if (tshut_value == table->data_mask)
		return -ERANGE;

	writel_relaxed(tshut_value, regs + TSADCV2_COMP_SHUT(chn));

	/* TSHUT will be valid */
	val = readl_relaxed(regs + TSADCV2_AUTO_CON);
	writel_relaxed(val | TSADCV2_AUTO_SRC_EN(chn), regs + TSADCV2_AUTO_CON);

	return 0;
}

static int rk_tsadcv3_tshut_temp(const struct chip_tsadc_table *table,
				 int chn, void __iomem *regs, int temp)
{
	u32 tshut_value;

	/* Make sure the value is valid */
	tshut_value = rk_tsadcv2_temp_to_code(table, temp);
	if (tshut_value == table->data_mask)
		return -ERANGE;

	writel_relaxed(tshut_value, regs + TSADCV3_COMP_SHUT(chn));

	/* TSHUT will be valid */
	writel_relaxed(TSADCV3_AUTO_SRC_EN(chn) | TSADCV3_AUTO_SRC_EN_MASK(chn),
		       regs + TSADCV3_AUTO_SRC_CON);

	return 0;
}

static void rk_tsadcv2_tshut_mode(int chn, void __iomem *regs,
				  enum tshut_mode mode)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_INT_EN);
	if (mode == TSHUT_MODE_GPIO) {
		val &= ~TSADCV2_SHUT_2CRU_SRC_EN(chn);
		val |= TSADCV2_SHUT_2GPIO_SRC_EN(chn);
	} else {
		val &= ~TSADCV2_SHUT_2GPIO_SRC_EN(chn);
		val |= TSADCV2_SHUT_2CRU_SRC_EN(chn);
	}

	writel_relaxed(val, regs + TSADCV2_INT_EN);
}

static void rk_tsadcv3_tshut_mode(int chn, void __iomem *regs,
				  enum tshut_mode mode)
{
	u32 val_gpio, val_cru;

	if (mode == TSHUT_MODE_GPIO) {
		val_gpio = TSADCV2_INT_SRC_EN(chn) | TSADCV2_INT_SRC_EN_MASK(chn);
		val_cru = TSADCV2_INT_SRC_EN_MASK(chn);
	} else {
		val_cru = TSADCV2_INT_SRC_EN(chn) | TSADCV2_INT_SRC_EN_MASK(chn);
		val_gpio = TSADCV2_INT_SRC_EN_MASK(chn);
	}
	writel_relaxed(val_gpio, regs + TSADCV3_HSHUT_GPIO_INT_EN);
	writel_relaxed(val_cru, regs + TSADCV3_HSHUT_CRU_INT_EN);
}

static const struct rockchip_tsadc_chip px30_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* 2 channels for tsadc */

	.tshut_mode = TSHUT_MODE_CRU, /* default TSHUT via CRU */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv4_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3328_code_table,
		.length = ARRAY_SIZE(rk3328_code_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rv1108_tsadc_data = {
	/* cpu */
	.chn_offset = 0,
	.chn_num = 1, /* one channel for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rv1108_table,
		.length = ARRAY_SIZE(rv1108_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3228_tsadc_data = {
	/* cpu */
	.chn_offset = 0,
	.chn_num = 1, /* one channel for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3228_code_table,
		.length = ARRAY_SIZE(rk3228_code_table),
		.data_mask = TSADCV3_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3288_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 1,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv2_irq_ack,
	.control = rk_tsadcv2_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3288_code_table,
		.length = ARRAY_SIZE(rk3288_code_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_DECREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3328_tsadc_data = {
	/* cpu */
	.chn_offset = 0,
	.chn_num = 1, /* one channels for tsadc */

	.tshut_mode = TSHUT_MODE_CRU, /* default TSHUT via CRU */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3328_code_table,
		.length = ARRAY_SIZE(rk3328_code_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3366_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv3_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3228_code_table,
		.length = ARRAY_SIZE(rk3228_code_table),
		.data_mask = TSADCV3_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3368_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv2_irq_ack,
	.control = rk_tsadcv2_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3368_code_table,
		.length = ARRAY_SIZE(rk3368_code_table),
		.data_mask = TSADCV3_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3399_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv3_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3399_code_table,
		.length = ARRAY_SIZE(rk3399_code_table),
		.data_mask = TSADCV3_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3568_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv7_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3568_code_table,
		.length = ARRAY_SIZE(rk3568_code_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3588_tsadc_data = {
	/* top, big_core0, big_core1, little_core, center, gpu, npu */
	.chn_offset = 0,
	.chn_num = 7, /* seven channels for tsadc */
	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,
	.initialize = rk_tsadcv8_initialize,
	.irq_ack = rk_tsadcv4_irq_ack,
	.control = rk_tsadcv4_control,
	.get_temp = rk_tsadcv4_get_temp,
	.set_alarm_temp = rk_tsadcv3_alarm_temp,
	.set_tshut_temp = rk_tsadcv3_tshut_temp,
	.set_tshut_mode = rk_tsadcv3_tshut_mode,
	.table = {
		.id = rk3588_code_table,
		.length = ARRAY_SIZE(rk3588_code_table),
		.data_mask = TSADCV4_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct of_device_id of_rockchip_thermal_match[] = {
	{	.compatible = "rockchip,px30-tsadc",
		.data = (void *)&px30_tsadc_data,
	},
	{
		.compatible = "rockchip,rv1108-tsadc",
		.data = (void *)&rv1108_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3228-tsadc",
		.data = (void *)&rk3228_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3288-tsadc",
		.data = (void *)&rk3288_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3328-tsadc",
		.data = (void *)&rk3328_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3366-tsadc",
		.data = (void *)&rk3366_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3368-tsadc",
		.data = (void *)&rk3368_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3399-tsadc",
		.data = (void *)&rk3399_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3568-tsadc",
		.data = (void *)&rk3568_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3588-tsadc",
		.data = (void *)&rk3588_tsadc_data,
	},
	{ /* end */ },
};
MODULE_DEVICE_TABLE(of, of_rockchip_thermal_match);

static void
rockchip_thermal_toggle_sensor(struct rockchip_thermal_sensor *sensor, bool on)
{
	struct thermal_zone_device *tzd = sensor->tzd;

	if (on)
		thermal_zone_device_enable(tzd);
	else
		thermal_zone_device_disable(tzd);
}

static irqreturn_t rockchip_thermal_alarm_irq_thread(int irq, void *dev)
{
	struct rockchip_thermal_data *thermal = dev;
	int i;

	dev_dbg(&thermal->pdev->dev, "thermal alarm\n");

	thermal->chip->irq_ack(thermal->regs);

	for (i = 0; i < thermal->chip->chn_num; i++)
		thermal_zone_device_update(thermal->sensors[i].tzd,
					   THERMAL_EVENT_UNSPECIFIED);

	return IRQ_HANDLED;
}

static int rockchip_thermal_set_trips(struct thermal_zone_device *tz, int low, int high)
{
	struct rockchip_thermal_sensor *sensor = thermal_zone_device_priv(tz);
	struct rockchip_thermal_data *thermal = sensor->thermal;
	const struct rockchip_tsadc_chip *tsadc = thermal->chip;

	dev_dbg(&thermal->pdev->dev, "%s: sensor %d: low: %d, high %d\n",
		__func__, sensor->id, low, high);

	return tsadc->set_alarm_temp(&tsadc->table,
				     sensor->id, thermal->regs, high);
}

static int rockchip_thermal_get_temp(struct thermal_zone_device *tz, int *out_temp)
{
	struct rockchip_thermal_sensor *sensor = thermal_zone_device_priv(tz);
	struct rockchip_thermal_data *thermal = sensor->thermal;
	const struct rockchip_tsadc_chip *tsadc = sensor->thermal->chip;
	int retval;

	retval = tsadc->get_temp(&tsadc->table,
				 sensor->id, thermal->regs, out_temp);
	return retval;
}

static const struct thermal_zone_device_ops rockchip_of_thermal_ops = {
	.get_temp = rockchip_thermal_get_temp,
	.set_trips = rockchip_thermal_set_trips,
};

static int rockchip_configure_from_dt(struct device *dev,
				      struct device_node *np,
				      struct rockchip_thermal_data *thermal)
{
	u32 shut_temp, tshut_mode, tshut_polarity;

	if (of_property_read_u32(np, "rockchip,hw-tshut-temp", &shut_temp)) {
		dev_warn(dev,
			 "Missing tshut temp property, using default %d\n",
			 thermal->chip->tshut_temp);
		thermal->tshut_temp = thermal->chip->tshut_temp;
	} else {
		if (shut_temp > INT_MAX) {
			dev_err(dev, "Invalid tshut temperature specified: %d\n",
				shut_temp);
			return -ERANGE;
		}
		thermal->tshut_temp = shut_temp;
	}

	if (of_property_read_u32(np, "rockchip,hw-tshut-mode", &tshut_mode)) {
		dev_warn(dev,
			 "Missing tshut mode property, using default (%s)\n",
			 thermal->chip->tshut_mode == TSHUT_MODE_GPIO ?
				"gpio" : "cru");
		thermal->tshut_mode = thermal->chip->tshut_mode;
	} else {
		thermal->tshut_mode = tshut_mode;
	}

	if (thermal->tshut_mode > 1) {
		dev_err(dev, "Invalid tshut mode specified: %d\n",
			thermal->tshut_mode);
		return -EINVAL;
	}

	if (of_property_read_u32(np, "rockchip,hw-tshut-polarity",
				 &tshut_polarity)) {
		dev_warn(dev,
			 "Missing tshut-polarity property, using default (%s)\n",
			 thermal->chip->tshut_polarity == TSHUT_LOW_ACTIVE ?
				"low" : "high");
		thermal->tshut_polarity = thermal->chip->tshut_polarity;
	} else {
		thermal->tshut_polarity = tshut_polarity;
	}

	if (thermal->tshut_polarity > 1) {
		dev_err(dev, "Invalid tshut-polarity specified: %d\n",
			thermal->tshut_polarity);
		return -EINVAL;
	}

	/* The tsadc wont to handle the error in here since some SoCs didn't
	 * need this property.
	 */
	thermal->grf = syscon_regmap_lookup_by_phandle(np, "rockchip,grf");
	if (IS_ERR(thermal->grf))
		dev_warn(dev, "Missing rockchip,grf property\n");

	return 0;
}

static int
rockchip_thermal_register_sensor(struct platform_device *pdev,
				 struct rockchip_thermal_data *thermal,
				 struct rockchip_thermal_sensor *sensor,
				 int id)
{
	const struct rockchip_tsadc_chip *tsadc = thermal->chip;
	int error;

	tsadc->set_tshut_mode(id, thermal->regs, thermal->tshut_mode);

	error = tsadc->set_tshut_temp(&tsadc->table, id, thermal->regs,
			      thermal->tshut_temp);
	if (error)
		dev_err(&pdev->dev, "%s: invalid tshut=%d, error=%d\n",
			__func__, thermal->tshut_temp, error);

	sensor->thermal = thermal;
	sensor->id = id;
	sensor->tzd = devm_thermal_of_zone_register(&pdev->dev, id, sensor,
						    &rockchip_of_thermal_ops);
	if (IS_ERR(sensor->tzd)) {
		error = PTR_ERR(sensor->tzd);
		dev_err(&pdev->dev, "failed to register sensor %d: %d\n",
			id, error);
		return error;
	}

	return 0;
}

/**
 * rockchip_thermal_reset_controller - Reset TSADC Controller, reset all tsadc registers.
 * @reset: the reset controller of tsadc
 */
static void rockchip_thermal_reset_controller(struct reset_control *reset)
{
	reset_control_assert(reset);
	usleep_range(10, 20);
	reset_control_deassert(reset);
}

static int rockchip_thermal_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct rockchip_thermal_data *thermal;
	int irq;
	int i;
	int error;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return -EINVAL;

	thermal = devm_kzalloc(&pdev->dev, sizeof(struct rockchip_thermal_data),
			       GFP_KERNEL);
	if (!thermal)
		return -ENOMEM;

	thermal->pdev = pdev;

	thermal->chip = device_get_match_data(&pdev->dev);
	if (!thermal->chip)
		return -EINVAL;

	thermal->sensors = devm_kcalloc(&pdev->dev, thermal->chip->chn_num,
					sizeof(*thermal->sensors), GFP_KERNEL);
	if (!thermal->sensors)
		return -ENOMEM;

	thermal->regs = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(thermal->regs))
		return PTR_ERR(thermal->regs);

	thermal->reset = devm_reset_control_array_get_exclusive(&pdev->dev);
	if (IS_ERR(thermal->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(thermal->reset),
				     "failed to get tsadc reset.\n");

	thermal->clk = devm_clk_get_enabled(&pdev->dev, "tsadc");
	if (IS_ERR(thermal->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(thermal->clk),
				     "failed to get tsadc clock.\n");

	thermal->pclk = devm_clk_get_enabled(&pdev->dev, "apb_pclk");
	if (IS_ERR(thermal->pclk))
		return dev_err_probe(&pdev->dev, PTR_ERR(thermal->pclk),
				     "failed to get apb_pclk clock.\n");

	rockchip_thermal_reset_controller(thermal->reset);

	error = rockchip_configure_from_dt(&pdev->dev, np, thermal);
	if (error)
		return dev_err_probe(&pdev->dev, error,
				"failed to parse device tree data\n");

	thermal->chip->initialize(thermal->grf, thermal->regs,
				  thermal->tshut_polarity);

	for (i = 0; i < thermal->chip->chn_num; i++) {
		error = rockchip_thermal_register_sensor(pdev, thermal,
						&thermal->sensors[i],
						thermal->chip->chn_offset + i);
		if (error)
			return dev_err_probe(&pdev->dev, error,
				"failed to register sensor[%d].\n", i);
	}

	error = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					  &rockchip_thermal_alarm_irq_thread,
					  IRQF_ONESHOT,
					  "rockchip_thermal", thermal);
	if (error)
		return dev_err_probe(&pdev->dev, error,
				     "failed to request tsadc irq.\n");

	thermal->chip->control(thermal->regs, true);

	for (i = 0; i < thermal->chip->chn_num; i++) {
		rockchip_thermal_toggle_sensor(&thermal->sensors[i], true);
		error = thermal_add_hwmon_sysfs(thermal->sensors[i].tzd);
		if (error)
			dev_warn(&pdev->dev,
				 "failed to register sensor %d with hwmon: %d\n",
				 i, error);
	}

	platform_set_drvdata(pdev, thermal);

	return 0;
}

static void rockchip_thermal_remove(struct platform_device *pdev)
{
	struct rockchip_thermal_data *thermal = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < thermal->chip->chn_num; i++) {
		struct rockchip_thermal_sensor *sensor = &thermal->sensors[i];

		thermal_remove_hwmon_sysfs(sensor->tzd);
		rockchip_thermal_toggle_sensor(sensor, false);
	}

	thermal->chip->control(thermal->regs, false);
}

static int __maybe_unused rockchip_thermal_suspend(struct device *dev)
{
	struct rockchip_thermal_data *thermal = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < thermal->chip->chn_num; i++)
		rockchip_thermal_toggle_sensor(&thermal->sensors[i], false);

	thermal->chip->control(thermal->regs, false);

	clk_disable(thermal->pclk);
	clk_disable(thermal->clk);

	pinctrl_pm_select_sleep_state(dev);

	return 0;
}

static int __maybe_unused rockchip_thermal_resume(struct device *dev)
{
	struct rockchip_thermal_data *thermal = dev_get_drvdata(dev);
	int i;
	int error;

	error = clk_enable(thermal->clk);
	if (error)
		return error;

	error = clk_enable(thermal->pclk);
	if (error) {
		clk_disable(thermal->clk);
		return error;
	}

	rockchip_thermal_reset_controller(thermal->reset);

	thermal->chip->initialize(thermal->grf, thermal->regs,
				  thermal->tshut_polarity);

	for (i = 0; i < thermal->chip->chn_num; i++) {
		int id = thermal->sensors[i].id;

		thermal->chip->set_tshut_mode(id, thermal->regs,
					      thermal->tshut_mode);

		error = thermal->chip->set_tshut_temp(&thermal->chip->table,
					      id, thermal->regs,
					      thermal->tshut_temp);
		if (error)
			dev_err(dev, "%s: invalid tshut=%d, error=%d\n",
				__func__, thermal->tshut_temp, error);
	}

	thermal->chip->control(thermal->regs, true);

	for (i = 0; i < thermal->chip->chn_num; i++)
		rockchip_thermal_toggle_sensor(&thermal->sensors[i], true);

	pinctrl_pm_select_default_state(dev);

	return 0;
}

static SIMPLE_DEV_PM_OPS(rockchip_thermal_pm_ops,
			 rockchip_thermal_suspend, rockchip_thermal_resume);

static struct platform_driver rockchip_thermal_driver = {
	.driver = {
		.name = "rockchip-thermal",
		.pm = &rockchip_thermal_pm_ops,
		.of_match_table = of_rockchip_thermal_match,
	},
	.probe = rockchip_thermal_probe,
	.remove = rockchip_thermal_remove,
};

module_platform_driver(rockchip_thermal_driver);

MODULE_DESCRIPTION("ROCKCHIP THERMAL Driver");
MODULE_AUTHOR("Rockchip, Inc.");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:rockchip-thermal");
