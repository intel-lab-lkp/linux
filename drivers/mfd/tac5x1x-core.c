// SPDX-License-Identifier: GPL-2.0
/*
 * tac5x1x-core.c -- Device access for TAC5x1x
 *
 * Copyright (C) 2025 Texas Instruments Incorporated
 *
 * Author: Niranjan H Y <niranjan.hy@ti.com>
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mfd/tac5x1x/core.h>
#include <linux/mfd/tac5x1x/registers.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

static const char * const tac5x1x_supply_names[TAC5X1X_NUM_SUPPLIES] = {
	"avdd",
	"iovdd",
};

static const struct regmap_range_cfg tac5x1x_ranges[] = {
	{
		.range_min = 0,
		.range_max = 12 * 128,
		.selector_reg = TAC_PAGE_SELECT,
		.selector_mask = GENMASK(7, 0),
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 128,
	},
};

static const struct regmap_range tac5x1x_volatile_ranges[] = {
	regmap_reg_range(TAC5X1X_RESET, TAC5X1X_RESET),
	regmap_reg_range(TAC5X1X_REG_INT_LTCH0, TAC5X1X_REG_INT_LTCH2),
};

static const struct regmap_access_table tac5x1x_volatile_table = {
	.yes_ranges = tac5x1x_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(tac5x1x_volatile_ranges),
};

/* Read-only (latch) registers */
static const struct regmap_range tac5x1x_read_only_ranges[] = {
	regmap_reg_range(TAC5X1X_REG_INT_LTCH0, TAC5X1X_REG_INT_LTCH2),
};

static const struct regmap_access_table tac5x1x_wr_table = {
	.no_ranges = tac5x1x_read_only_ranges,
	.n_no_ranges = ARRAY_SIZE(tac5x1x_read_only_ranges),
};

static const struct regmap_config tac5x1x_regmap = {
	.max_register = 12 * 128,
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_MAPLE,
	.ranges = tac5x1x_ranges,
	.num_ranges = ARRAY_SIZE(tac5x1x_ranges),
	.volatile_table = &tac5x1x_volatile_table,
	.wr_table = &tac5x1x_wr_table,
};

static int tac5x1x_reset(struct tac5x1x *tac5x1x)
{
	int ret;

	ret = regmap_write(tac5x1x->regmap, TAC5X1X_RESET, 1);
	if (ret < 0)
		return ret;
	/* Wait >= 10 ms after entering sleep mode. */
	usleep_range(10000, 100000);
	regcache_mark_dirty(tac5x1x->regmap);

	return ret;
}

static int tac5x1x_suspend(struct device *dev)
{
	struct tac5x1x *tac5x1x = dev_get_drvdata(dev);

	regcache_cache_only(tac5x1x->regmap, true);
	regcache_mark_dirty(tac5x1x->regmap);

	/* Only disable regulators if they are currently enabled */
	if (tac5x1x->regulators_enabled) {
		regulator_bulk_disable(TAC5X1X_NUM_SUPPLIES, tac5x1x->supplies);
		tac5x1x->regulators_enabled = false;
	}

	return 0;
}

static int tac5x1x_resume(struct device *dev)
{
	struct tac5x1x *tac5x1x = dev_get_drvdata(dev);
	int ret;

	/* Only enable regulators if they are not already enabled */
	if (!tac5x1x->regulators_enabled) {
		ret = regulator_bulk_enable(TAC5X1X_NUM_SUPPLIES, tac5x1x->supplies);
		if (ret) {
			dev_err(dev, "Failed to enable regulators: %d\n", ret);
			return ret;
		}
		tac5x1x->regulators_enabled = true;
	}

	regcache_cache_only(tac5x1x->regmap, false);
	ret = regcache_sync(tac5x1x->regmap);
	if (ret) {
		dev_err(dev, "Failed to restore register map: %d\n", ret);
		regulator_bulk_disable(TAC5X1X_NUM_SUPPLIES, tac5x1x->supplies);
		tac5x1x->regulators_enabled = false;
		return ret;
	}

	return 0;
}

static const int vref_uv[] = {
	2750000,
	2500000,
	1375000,
};

static int voltage_to_vref_cfg(int voltage_uv)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vref_uv); i++) {
		if (vref_uv[i] == voltage_uv)
			return i;
	}

	return -EINVAL;
}

static const int adc_impedance_ohms[] = {
	5000,  /* Index 0: 5 kΩ */
	10000, /* Index 1: 10 kΩ */
	40000, /* Index 2: 40 kΩ */
};

static int impedance_to_cfg(int impedance_ohms)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(adc_impedance_ohms); i++)
		if (adc_impedance_ohms[i] == impedance_ohms)
			return i;
	return -EINVAL;
}

static int micbias_thr_current_ua_to_reg(int current_ua)
{
	s64 temp;
	int n;

	/*
	 * N = ((current_uA × 10 / 48461.54) + 2) × 4095 / 144
	 *  x 10 to remove the decimal point on both sides
	 * Rearrange: N = (current_uA × 10 × 4095) / (48461.54 × 144) + (2 × 4095 / 144)
	 * N = (current_uA × 40950) / 6978461.76 + 56.875
	 */

	/* First term: (current_uA × 40950) / 6978461.76 */
	temp = (s64)current_ua * 40950; /* × 40950 */
	temp = div_s64(temp, 6978462);	/* / 6978461.76 ≈ 6978462 */
	/* Second term: 56.875 ≈ 57 */
	temp += 57;

	n = (int)temp;
	n = clamp(n, 0, 255);

	return n;
}

static int tac5x1x_validate_gpa_thresholds(struct tac5x1x *tac5x1x,
					   int *thresholds_mv)
{
	int orig_low = thresholds_mv[0];
	int orig_high = thresholds_mv[1];

	/* Clamp thresholds to valid ranges */
	thresholds_mv[0] = clamp(thresholds_mv[0], 0, 6000);
	thresholds_mv[1] = clamp(thresholds_mv[1], 0, 6000);

	if (thresholds_mv[0] >= thresholds_mv[1]) {
		dev_err(tac5x1x->dev,
			"Invalid GPA thresholds: low(%dmV) must be < high(%dmV)\n",
			thresholds_mv[0], thresholds_mv[1]);
		return -EINVAL;
	}

	/* Warn if values were clamped */
	if (orig_low != thresholds_mv[0]) {
		dev_warn(tac5x1x->dev,
			 "GPA low threshold clamped from %dmV to %dmV\n",
			 orig_low, thresholds_mv[0]);
	}
	if (orig_high != thresholds_mv[1]) {
		dev_warn(tac5x1x->dev,
			 "GPA high threshold clamped from %dmV to %dmV\n",
			 orig_high, thresholds_mv[1]);
	}

	return 0;
}

/* Voltage conversion functions (from previous discussion) */
static int voltage_mv_to_register_value(int voltage_mv)
{
	s64 temp;
	int n;

	/* Clamp input to reasonable range */
	voltage_mv = clamp(voltage_mv, 0, 6000);

	/*
	 * Formula: nd = ((0.9*(N*16)/4095) - 0.225) × 6 (V)
	 * Solving for N: N = ((voltage_V/6 + 0.225) × 4095) / 14.4
	 */

	temp = (s64)voltage_mv * 1000000; /* Scale for precision */
	temp = div_s64(temp, 6000);	  /* voltage_mV / 6000 */
	temp += 225000;			  /* + 0.225 * 1000000 */
	temp = temp * 4095;		  /* × 4095 */
	temp = div_s64(temp, 14400000);	  /* / (14.4 * 1000000) */

	n = (int)temp;

	return clamp(n, 0, 255);
}

static int tac5x1x_parse_gpa_thresholds(struct tac5x1x *tac5x1x)
{
	int thresholds_mv[2] = {200, 2600}; /* TI datasheet defaults: 0.2V, 2.6V */
	int ret;

	/* Read thresholds from device tree */
	ret = fwnode_property_read_u32_array(tac5x1x->dev->fwnode,
					     "ti,gpa-threshold",
					     thresholds_mv, 2);
	if (ret)
		dev_dbg(tac5x1x->dev,
			"Using default GPA thresholds: [%d, %d] mV\n",
			thresholds_mv[0], thresholds_mv[1]);

	/* Validate threshold values */
	ret = tac5x1x_validate_gpa_thresholds(tac5x1x, thresholds_mv);
	if (ret)
		return ret;

	/* Convert mV values to register values */
	tac5x1x->gpa_threshold[0] = voltage_mv_to_register_value(thresholds_mv[0]);
	tac5x1x->gpa_threshold[1] = voltage_mv_to_register_value(thresholds_mv[1]);

	dev_dbg(tac5x1x->dev, "GPA thresholds: %dmV->0x%02x, %dmV->0x%02x\n",
		thresholds_mv[0], tac5x1x->gpa_threshold[0],
		thresholds_mv[1], tac5x1x->gpa_threshold[1]);

	return 0;
}

static int tac5x1x_parse_dt(struct tac5x1x *tac5x1x,
			    struct device_node *np)
{
	struct regulator *avdd_reg;
	struct tac5x1x_input_diag_config input_config = {};
	int vref_voltage_uv = 2500000; /* Default 2.5V */
	int micbias_voltage_uv = 0;
	int vref_cfg, micbias_cfg;
	int ret;
	int avdd_uv;
	int adc_impedance_ohms;
	int micbias_thr_uv[2];

	avdd_reg = devm_regulator_get(tac5x1x->dev, "avdd");
	if (IS_ERR(avdd_reg)) {
		dev_err(tac5x1x->dev, "Failed to get avdd regulator: %ld\n", PTR_ERR(avdd_reg));
		return PTR_ERR(avdd_reg);
	}

	avdd_uv = regulator_get_voltage(avdd_reg);
	if (avdd_uv < 0) {
		dev_err(tac5x1x->dev, "Failed to get avdd voltage: %d\n", avdd_uv);
		ret = -EINVAL;
		goto out;
	}

	/* Read VREF voltage directly */
	ret = fwnode_property_read_u32(tac5x1x->dev->fwnode, "ti,vref-voltage",
				       &vref_voltage_uv);
	if (ret)
		dev_dbg(tac5x1x->dev, "Using default vref-voltage: %duV\n", vref_voltage_uv);

	/* Validate VREF voltage is one of the supported values */
	vref_cfg = voltage_to_vref_cfg(vref_voltage_uv);
	if (vref_cfg < 0) {
		dev_err(tac5x1x->dev, "Invalid vref-voltage %duV. Supported: 1375000, 2500000, 2750000\n",
			vref_voltage_uv);
		ret = -EINVAL;
		goto out;
	}

	/* Validate VREF is lower than AVDD */
	if (vref_voltage_uv >= avdd_uv) {
		dev_err(tac5x1x->dev, "vref-voltage (%duV) must be lower than avdd-voltage (%duV)\n",
			vref_voltage_uv, avdd_uv);
		ret = -EINVAL;
		goto out;
	}

	/* Read micbias voltage directly */
	ret = fwnode_property_read_u32(tac5x1x->dev->fwnode,
				       "ti,micbias-voltage",
				       &micbias_voltage_uv);
	if (ret) {
		micbias_voltage_uv = vref_voltage_uv;
		dev_dbg(tac5x1x->dev,
			"Using default micbias-voltage: %duV\n",
			micbias_voltage_uv);
		ret = 0;
	}

	if (micbias_voltage_uv == avdd_uv) {
		micbias_cfg = TAC5X1X_MICBIAS_AVDD;
		vref_cfg = TAC5X1X_VERF_2_75V; /* VREF to 2.75V for AVDD */
		dev_dbg(tac5x1x->dev,
			"micbias set to AVDD (%duV),forcing VREF to 2.75V\n",
			avdd_uv);
	} else if (micbias_voltage_uv == vref_voltage_uv) {
		micbias_cfg = TAC5X1X_MICBIAS_VREF;
	} else if (micbias_voltage_uv == (vref_voltage_uv / 2)) {
		micbias_cfg = TAC5X1X_MICBIAS_0_5VREF;
	} else {
		dev_err(tac5x1x->dev,
			"Invalid micbias %duV. Must be %duV, %duV, or %duV\n",
			micbias_voltage_uv,
			vref_voltage_uv / 2,
			vref_voltage_uv, avdd_uv);
		ret = -EINVAL;
		goto out;
	}

	/* Store the validated configurations */
	tac5x1x->vref_vg = vref_cfg;
	tac5x1x->micbias_vg = micbias_cfg;

	dev_dbg(tac5x1x->dev,
		"VREF: %duV, Micbias: %duV, AVDD: %duV\n",
		vref_voltage_uv, micbias_voltage_uv, avdd_uv);

	tac5x1x->adc_impedance[0] = -1;
	tac5x1x->adc_impedance[1] = -1;
	tac5x1x->out2x_vcom_cfg = -1;

	tac5x1x->gpa_threshold[0] = TAC5X1X_GPA_LOW_THRESHOLD;
	tac5x1x->gpa_threshold[1] = TAC5X1X_GPA_HIGH_THRESHOLD;

	ret = tac5x1x_parse_gpa_thresholds(tac5x1x);
	if (ret)
		return ret;

	fwnode_property_read_u32(tac5x1x->dev->fwnode, "ti,out2x-vcom-cfg",
				 &tac5x1x->out2x_vcom_cfg);

	switch (tac5x1x->codec_type) {
	case TAA5212:
	case TAC5212:
		if (!fwnode_property_read_u32(tac5x1x->dev->fwnode,
					      "ti,adc2-impedance",
					      &adc_impedance_ohms)) {
			int adc2_cfg = impedance_to_cfg(adc_impedance_ohms);

			if (adc2_cfg < 0) {
				dev_err(tac5x1x->dev,
					"Invalid adc2-impedance %dOhm. Supported: 5000, 10000, 40000\n",
					adc_impedance_ohms);
				ret = -EINVAL;
				goto out;
			}
			tac5x1x->adc_impedance[1] = adc2_cfg;
			dev_dbg(tac5x1x->dev, "ADC2 impedance: %dOhm (cfg=%d)\n",
				adc_impedance_ohms, adc2_cfg);
		}
		fallthrough;
	case TAC5211:
	case TAC5111:
		if (!fwnode_property_read_u32(tac5x1x->dev->fwnode,
					      "ti,adc1-impedance",
					      &adc_impedance_ohms)) {
			int adc1_cfg = impedance_to_cfg(adc_impedance_ohms);

			if (adc1_cfg < 0) {
				dev_err(tac5x1x->dev,
					"Invalid adc1-impedance %dOhm. Supported: 5000, 10000, 40000\n",
					adc_impedance_ohms);
				ret = -EINVAL;
				goto out;
			}
			tac5x1x->adc_impedance[0] = adc1_cfg;
			dev_dbg(tac5x1x->dev, "ADC1 impedance: %dOhm (cfg=%d)\n",
				adc_impedance_ohms, adc1_cfg);
		}
		fallthrough;
	case TAC5112:
	case TAD5112:
	case TAD5212:
		break;
	case TAA5412:
	case TAC5411:
	case TAC5412:
	case TAC5301:
	case TAC5311:
	case TAC5312:
		tac5x1x->input_diag_config.in_ch_en = 0;
		if (fwnode_property_read_u32(tac5x1x->dev->fwnode,
					     "ti,in-ch-en",
					     &input_config.in_ch_en))
			dev_dbg(tac5x1x->dev,
				"Fail to get ti,in-ch-en value\n");
		tac5x1x->input_diag_config.out_ch_en = 0;
		if (fwnode_property_read_u32(tac5x1x->dev->fwnode,
					     "ti,out-ch-en",
					     &input_config.in_ch_en))
			dev_dbg(tac5x1x->dev,
				"Fail to get ti,out-ch-en value\n");
		tac5x1x->input_diag_config.incl_se_inm = 0;
		if (fwnode_property_read_u32(tac5x1x->dev->fwnode,
					     "ti,incl-se-inm",
					     &input_config.incl_se_inm))
			dev_dbg(tac5x1x->dev,
				"Fail to get ti,incl-se-inm value\n");
		tac5x1x->input_diag_config.incl_ac_coup = 0;
		if (fwnode_property_read_u32(tac5x1x->dev->fwnode,
					     "ti,incl-ac-coup",
					     &input_config.incl_ac_coup))
			dev_dbg(tac5x1x->dev,
				"Fail to get ti,incl-ac-coup value\n");
		tac5x1x->input_diag_config = input_config;

		if (fwnode_property_read_u32_array(tac5x1x->dev->fwnode,
						   "ti,micbias-threshold",
						   micbias_thr_uv, 2)) {
			tac5x1x->micbias_thr[0] = -1;
			tac5x1x->micbias_thr[1] = -1;
			dev_dbg(tac5x1x->dev,
				"ignoring micbias threshold propert read err\n");
		} else {
			tac5x1x->micbias_thr[0] =
				micbias_thr_current_ua_to_reg(micbias_thr_uv[0]);
			tac5x1x->micbias_thr[1] =
				micbias_thr_current_ua_to_reg(micbias_thr_uv[1]);
		}
		break;
	}
out:
	return ret;
}

static int tac5x1x_init(struct tac5x1x *tac5x1x)
{
	struct regmap *regmap = tac5x1x->regmap;

	/* ADC Channels input coupling configuration */
	regmap_write(regmap, TAC5X1X_ADCCH1C0, 0x04);
	regmap_write(regmap, TAC5X1X_ADCCH2C0, 0x04);

	/* Disable inputs and outputs */
	regmap_write(regmap, TAC5X1X_CH_EN, 0x00);
	regmap_write(regmap, TAC5X1X_PASITXCH1, 0x00);
	regmap_write(regmap, TAC5X1X_PASITXCH2, 0x01);
	regmap_write(regmap, TAC5X1X_PASIRXCH1, 0x00);
	regmap_write(regmap, TAC5X1X_PASIRXCH2, 0x01);

	/* clear latch irrespctive of live status */
	regmap_write(regmap, TAC5X1X_INT, 0x11);

	return 0;
}

static const struct mfd_cell tac5x1x_mfd_devs[] = {
	{
		.id = 0,
		.name = "tac5x1x-pinctrl",
		.pm_runtime_no_callbacks = true,
	},
	{
		.id = 1,
		.name = "tac5x1x-codec",
	}
};

static int tac5x1x_setup_regulators(struct device *dev,
				    struct tac5x1x *tac5x1x)
{
	int i, ret;

	for (i = 0; i < TAC5X1X_NUM_SUPPLIES; i++)
		tac5x1x->supplies[i].supply = tac5x1x_supply_names[i];

	ret = devm_regulator_bulk_get(dev, TAC5X1X_NUM_SUPPLIES,
				      tac5x1x->supplies);
	if (ret) {
		dev_err(dev, "Failed to get regulators: %d\n", ret);
		return ret;
	}

	/* Regulators managed by PM runtime during probe */
	tac5x1x->regulators_enabled = false;

	return 0;
}

static int tac5x1x_probe(struct device *dev, struct tac5x1x *tac5x1x)
{
	struct device_node *np = dev->of_node;
	int ret;

	ret = tac5x1x_setup_regulators(dev, tac5x1x);
	if (ret)
		return ret;

	/* Initialize PM runtime before adding child devices */
	pm_runtime_set_autosuspend_delay(dev, 3000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	pm_runtime_mark_last_busy(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	/* Enable regulators for device initialization */
	ret = regulator_bulk_enable(TAC5X1X_NUM_SUPPLIES, tac5x1x->supplies);
	if (ret) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}
	tac5x1x->regulators_enabled = true;

	ret = tac5x1x_reset(tac5x1x);
	if (ret) {
		dev_err(dev, "Failed to reset device\n");
		goto err_disable_regulators;
	}
	tac5x1x_init(tac5x1x);

	ret = tac5x1x_parse_dt(tac5x1x, np);
	if (ret) {
		dev_err(dev, "Failed to parse DT node: %d\n", ret);
		goto err_disable_regulators;
	}

	/* update if vcom property is found */
	if (tac5x1x->out2x_vcom_cfg != -1) {
		regmap_update_bits(tac5x1x->regmap, TAC5X1X_OUT2CFG0,
				   TAC5X1X_OUT2CFG0_VCOM_MASK,
				   tac5x1x->out2x_vcom_cfg);
	}

	dev_dbg(dev, "%s adding mfds\n", __func__);

	/* Add child devices now PM runtime is initialized */
	ret = mfd_add_devices(dev, PLATFORM_DEVID_NONE, tac5x1x_mfd_devs,
			      ARRAY_SIZE(tac5x1x_mfd_devs), NULL, 0, NULL);
	if (ret) {
		dev_err(dev, "Failed to add mfd devices\n");
		goto err_remove_mfd;
	}

	return 0;

err_remove_mfd:
	mfd_remove_devices(dev);
err_disable_regulators:
	if (tac5x1x->regulators_enabled) {
		regulator_bulk_disable(TAC5X1X_NUM_SUPPLIES, tac5x1x->supplies);
		tac5x1x->regulators_enabled = false;
	}
	return ret;
}

static void tac5x1x_remove(struct tac5x1x *tac5x1x)
{
	mfd_remove_devices(tac5x1x->dev);
	/* Only disable regulators if they are still enabled */
	if (tac5x1x->regulators_enabled) {
		regulator_bulk_disable(TAC5X1X_NUM_SUPPLIES, tac5x1x->supplies);
		tac5x1x->regulators_enabled = false;
	}
}

const struct of_device_id tac5x1x_of_match[] = {
	{ .compatible = "ti,taa5212", .data = (void *)TAA5212 },
	{ .compatible = "ti,taa5412", .data = (void *)TAA5412 },
	{ .compatible = "ti,tac5111", .data = (void *)TAC5111 },
	{ .compatible = "ti,tac5112", .data = (void *)TAC5112 },
	{ .compatible = "ti,tac5211", .data = (void *)TAC5211 },
	{ .compatible = "ti,tac5212", .data = (void *)TAC5212 },
	{ .compatible = "ti,tac5301", .data = (void *)TAC5301 },
	{ .compatible = "ti,tac5311", .data = (void *)TAC5311 },
	{ .compatible = "ti,tac5312", .data = (void *)TAC5312 },
	{ .compatible = "ti,tac5411", .data = (void *)TAC5411 },
	{ .compatible = "ti,tac5412", .data = (void *)TAC5412 },
	{ .compatible = "ti,tad5112", .data = (void *)TAD5112 },
	{ .compatible = "ti,tad5212", .data = (void *)TAD5212 },
	{}
};
MODULE_DEVICE_TABLE(of, tac5x1x_of_match);

static int tac5x1x_i2c_probe(struct i2c_client *i2c)
{
	enum tac5x1x_type type;
	struct tac5x1x *tac5x1x;
	struct regmap *regmap;
	const struct regmap_config *config = &tac5x1x_regmap;

	tac5x1x = devm_kzalloc(&i2c->dev, sizeof(struct tac5x1x),
			       GFP_KERNEL);
	if (!tac5x1x)
		return -ENOMEM;

	i2c_set_clientdata(i2c, tac5x1x);
	regmap = devm_regmap_init_i2c(i2c, config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);
	type = (uintptr_t)i2c_get_match_data(i2c);

	tac5x1x->dev = &i2c->dev;
	tac5x1x->codec_type = type;
	tac5x1x->regmap = regmap;

	return tac5x1x_probe(&i2c->dev, tac5x1x);
}

static void tac5x1x_i2c_remove(struct i2c_client *client)
{
	tac5x1x_remove(i2c_get_clientdata(client));
}

static const struct i2c_device_id tac5x1x_id[] = {
	{"taa5212", TAA5212},
	{"taa5412", TAA5412},
	{"tac5111", TAC5111},
	{"tac5112", TAC5112},
	{"tac5211", TAC5211},
	{"tac5212", TAC5212},
	{"tac5301", TAC5301},
	{"tac5311", TAC5311},
	{"tac5312", TAC5312},
	{"tac5411", TAC5411},
	{"tac5412", TAC5412},
	{"tad5112", TAD5112},
	{"tad5212", TAD5212},
	{ }
};
MODULE_DEVICE_TABLE(i2c, tac5x1x_id);

static DEFINE_RUNTIME_DEV_PM_OPS(tac5x1x_pm_ops, tac5x1x_suspend,
				 tac5x1x_resume, NULL);

static struct i2c_driver tac5x1x_i2c_driver = {
	.driver = {
		.name = "tac5x1x-core",
		.pm = pm_ptr(&tac5x1x_pm_ops),
		.of_match_table = of_match_ptr(tac5x1x_of_match),
	},
	.probe = tac5x1x_i2c_probe,
	.remove = tac5x1x_i2c_remove,
	.id_table = tac5x1x_id,
};
module_i2c_driver(tac5x1x_i2c_driver);

MODULE_DESCRIPTION("Core support for the ASoC tac5x1x codec driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Niranjan H Y <niranjan.hy@ti.com>");
MODULE_SOFTDEP("pre: tac5x1x_pinctrl");
