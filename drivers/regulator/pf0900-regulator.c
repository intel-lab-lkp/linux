// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 NXP.
 * NXP PF0900 pmic driver
 */

#include <linux/crc8.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/pf0900.h>

struct pf0900_dvs_config {
	unsigned int run_reg;
	unsigned int run_mask;
	unsigned int standby_reg;
	unsigned int standby_mask;
};

struct pf0900_regulator_desc {
	struct regulator_desc desc;
	const struct pf0900_dvs_config dvs;
};

struct pf0900_drvdata {
	const struct pf0900_regulator_desc *desc;
	enum pf0900_chip_type type;
	unsigned int rcnt;
};

struct pf0900 {
	struct device *dev;
	struct regmap *regmap;
	const struct pf0900_drvdata *drvdata;
	int irq;
	unsigned short addr;
	bool crc_en;
};

static const struct regmap_range pf0900_range = {
	.range_min = PF0900_REG_DEV_ID,
	.range_max = PF0900_REG_SYS_DIAG,
};

static const struct regmap_access_table pf0900_volatile_regs = {
	.yes_ranges = &pf0900_range,
	.n_yes_ranges = 1,
};

static const struct regmap_config pf0900_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_table = &pf0900_volatile_regs,
	.max_register = PF0900_MAX_REGISTER - 1,
	.cache_type = REGCACHE_RBTREE,
};

static uint8_t crc8_j1850(unsigned short addr, unsigned int reg,
			  unsigned int val)
{
	uint8_t crcBuf[3];
	uint8_t t_crc;
	uint8_t i, j;

	crcBuf[0] = addr;
	crcBuf[1] = reg;
	crcBuf[2] = val;
	t_crc = 0xFF;

	/*
	 * The CRC calculation is based on the standard CRC-8-SAE as
	 * defined in the SAE-J1850 specification with the following
	 * characteristics.
	 * Polynomial = 0x1D
	 * Initial Value = 0xFF
	 * The CRC byte is calculated by shifting 24-bit data through
	 * the CRC polynomial.The 24-bits package is built as follows:
	 * DEVICE_ADDR[b8] + REGISTER_ADDR [b8] +DATA[b8]
	 * The DEVICE_ADDR is calculated as the 7-bit slave address
	 * shifted left one space plus the corresponding read/write bit.
	 * (7Bit Address [b7] << 1 ) + R/W = DEVICE_ADDR[b8]
	 */
	for (i = 0; i < sizeof(crcBuf); i++) {
		t_crc ^= crcBuf[i];
		for (j = 0; j < 8; j++) {
			if ((t_crc & 0x80) != 0) {
				t_crc <<= 1;
				t_crc ^= 0x1D;
			} else {
				t_crc <<= 1;
			}
		}
	}
	return t_crc;
}

static int pf0900_pmic_read(struct pf0900 *pf0900, unsigned int reg,
			    unsigned int *val)
{
	u8 data[2], crc;
	int ret;

	if (!pf0900 || !pf0900->dev)
		return -EINVAL;

	if (reg >= PF0900_MAX_REGISTER) {
		dev_err(pf0900->dev, "Invalid register address: 0x%x\n", reg);
		return -EINVAL;
	}

	ret = regmap_raw_read(pf0900->regmap, reg, data,
			      pf0900->crc_en ? 2 : 1);
	if (ret) {
		dev_err(pf0900->dev, "Read reg=0x%x error!\n", reg);
		return ret;
	}
	*val = data[0];
	if (pf0900->crc_en) {
		/* Get CRC */
		crc = crc8_j1850(pf0900->addr << 1 | 0x1, reg, data[0]);
		if (crc != data[1])
			return -EINVAL;
	}
	return 0;
}

static int pf0900_pmic_write(struct pf0900 *pf0900, unsigned int reg,
			     unsigned int val, uint8_t mask)
{
	unsigned int rxBuf;
	uint8_t data[2];
	int ret;

	if (!pf0900 || !pf0900->dev)
		return -EINVAL;

	if (reg >= PF0900_MAX_REGISTER) {
		dev_err(pf0900->dev, "Invalid register address: 0x%x\n", reg);
		return -EINVAL;
	}

	/* If not updating entire register, perform a read-mod-write */
	data[0] = val;

	if (mask != 0xFF) {
		/* Read data */
		ret = pf0900_pmic_read(pf0900, reg, &rxBuf);
		if (ret) {
			dev_err(pf0900->dev, "Read reg=0x%x error!\n", reg);
			return ret;
		}
		data[0] = (val & mask) | (rxBuf & (~mask));
	}

	if (pf0900->crc_en)
		/* Get CRC */
		data[1] = crc8_j1850(pf0900->addr << 1, reg, data[0]);

	/* Write data */
	ret = regmap_raw_write(pf0900->regmap, reg, data,
			       pf0900->crc_en ? 2 : 1);
	if (ret) {
		dev_err(pf0900->dev, "Write reg=0x%x error!\n", reg);
		return ret;
	}

	return 0;
}

/**
 * pf0900_regulator_enable_regmap for regmap users
 *
 * @rdev: regulator to operate on
 *
 * Regulators that use regmap for their register I/O can set the
 * enable_reg and enable_mask fields in their descriptor and then use
 * this as their enable() operation, saving some code.
 */
static int pf0900_regulator_enable_regmap(struct regulator_dev *rdev)
{
	struct pf0900 *pf0900 = dev_get_drvdata(rdev->dev.parent);
	unsigned int val;

	if (rdev->desc->enable_is_inverted) {
		val = rdev->desc->disable_val;
	} else {
		val = rdev->desc->enable_val;
		if (!val)
			val = rdev->desc->enable_mask;
	}

	return pf0900_pmic_write(pf0900, rdev->desc->enable_reg, val,
				 rdev->desc->enable_mask);
}

/**
 * pf0900_regulator_disable_regmap for regmap users
 *
 * @rdev: regulator to operate on
 *
 * Regulators that use regmap for their register I/O can set the
 * enable_reg and enable_mask fields in their descriptor and then use
 * this as their disable() operation, saving some code.
 */
static int pf0900_regulator_disable_regmap(struct regulator_dev *rdev)
{
	struct pf0900 *pf0900 = dev_get_drvdata(rdev->dev.parent);
	unsigned int val;

	if (rdev->desc->enable_is_inverted) {
		val = rdev->desc->enable_val;
		if (!val)
			val = rdev->desc->enable_mask;
	} else {
		val = rdev->desc->disable_val;
	}

	return pf0900_pmic_write(pf0900, rdev->desc->enable_reg, val,
				 rdev->desc->enable_mask);
}

/**
 * pf0900_regulator_is_enabled_regmap for regmap users
 *
 * @rdev: regulator to operate on
 *
 * Regulators that use regmap for their register I/O can set the
 * enable_reg and enable_mask fields in their descriptor and then use
 * this as their is_enabled operation, saving some code.
 */
static int pf0900_regulator_is_enabled_regmap(struct regulator_dev *rdev)
{
	struct pf0900 *pf0900 = dev_get_drvdata(rdev->dev.parent);
	unsigned int val;
	int ret;

	ret = pf0900_pmic_read(pf0900, rdev->desc->enable_reg, &val);
	if (ret != 0)
		return ret;

	val &= rdev->desc->enable_mask;

	if (rdev->desc->enable_is_inverted) {
		if (rdev->desc->enable_val)
			return val != rdev->desc->enable_val;
		return val == 0;
	} else {
		if (rdev->desc->enable_val)
			return val == rdev->desc->enable_val;
		return val != 0;
	}
}

/**
 * pf0900_regulator_set_voltage_sel_regmap for regmap users
 *
 * @rdev: regulator to operate on
 * @sel: Selector to set
 *
 * Regulators that use regmap for their register I/O can set the
 * vsel_reg and vsel_mask fields in their descriptor and then use this
 * as their set_voltage_vsel operation, saving some code.
 */
static int pf0900_regulator_set_voltage_sel_regmap(struct regulator_dev *rdev,
					    unsigned int sel)
{
	struct pf0900 *pf0900 = dev_get_drvdata(rdev->dev.parent);
	int ret;

	sel <<= ffs(rdev->desc->vsel_mask) - 1;
	ret = pf0900_pmic_write(pf0900, rdev->desc->vsel_reg, sel,
				rdev->desc->vsel_mask);
	if (ret)
		return ret;

	if (rdev->desc->apply_bit)
		ret = pf0900_pmic_write(pf0900, rdev->desc->apply_reg,
					rdev->desc->apply_bit,
					rdev->desc->apply_bit);
	return ret;
}

static int find_closest_bigger(unsigned int target, const unsigned int *table,
			       unsigned int num_sel, unsigned int *sel)
{
	unsigned int s, tmp, max, maxsel = 0;
	bool found = false;

	max = table[0];

	for (s = 0; s < num_sel; s++) {
		if (table[s] > max) {
			max = table[s];
			maxsel = s;
		}
		if (table[s] >= target) {
			if (!found || table[s] - target < tmp - target) {
				tmp = table[s];
				*sel = s;
				found = true;
				if (tmp == target)
					break;
			}
		}
	}

	if (!found) {
		*sel = maxsel;
		return -EINVAL;
	}

	return 0;
}

/**
 * pf0900_regulator_set_ramp_delay_regmap
 *
 * @rdev: regulator to operate on
 * @ramp_delay: desired ramp delay value in microseconds
 *
 * Regulators that use regmap for their register I/O can set the ramp_reg
 * and ramp_mask fields in their descriptor and then use this as their
 * set_ramp_delay operation, saving some code.
 */
static int pf0900_regulator_set_ramp_delay_regmap(struct regulator_dev *rdev,
					   int ramp_delay)
{
	struct pf0900 *pf0900 = dev_get_drvdata(rdev->dev.parent);
	unsigned int sel;
	int ret;

	if (WARN_ON(!rdev->desc->n_ramp_values || !rdev->desc->ramp_delay_table))
		return -EINVAL;

	ret = find_closest_bigger(ramp_delay, rdev->desc->ramp_delay_table,
				  rdev->desc->n_ramp_values, &sel);

	if (ret) {
		dev_warn(rdev_get_dev(rdev),
			 "Can't set ramp-delay %u, setting %u\n", ramp_delay,
			 rdev->desc->ramp_delay_table[sel]);
	}

	sel <<= ffs(rdev->desc->ramp_mask) - 1;

	return pf0900_pmic_write(pf0900, rdev->desc->ramp_reg, sel,
				 rdev->desc->ramp_mask);
}

/**
 * pf0900_regulator_get_voltage_sel_regmap for regmap users
 *
 * @rdev: regulator to operate on
 *
 * Regulators that use regmap for their register I/O can set the
 * vsel_reg and vsel_mask fields in their descriptor and then use this
 * as their get_voltage_vsel operation, saving some code.
 */
static int pf0900_regulator_get_voltage_sel_regmap(struct regulator_dev *rdev)
{
	struct pf0900 *pf0900 = dev_get_drvdata(rdev->dev.parent);
	unsigned int val;
	int ret;

	ret = pf0900_pmic_read(pf0900, rdev->desc->vsel_reg, &val);
	if (ret != 0)
		return ret;

	val &= rdev->desc->vsel_mask;
	val >>= ffs(rdev->desc->vsel_mask) - 1;

	return val;
}

static const struct regulator_ops pf0900_avon_regulator_ops = {
	.list_voltage = regulator_list_voltage_table,
	.set_voltage_sel = pf0900_regulator_set_voltage_sel_regmap,
	.get_voltage_sel = pf0900_regulator_get_voltage_sel_regmap,
};

static const struct regulator_ops pf0900_dvs_sw_regulator_ops = {
	.enable = pf0900_regulator_enable_regmap,
	.disable = pf0900_regulator_disable_regmap,
	.is_enabled = pf0900_regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear_range,
	.set_voltage_sel = pf0900_regulator_set_voltage_sel_regmap,
	.get_voltage_sel = pf0900_regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.set_ramp_delay	= pf0900_regulator_set_ramp_delay_regmap,
};

static const struct regulator_ops pf0900_ldo_regulator_ops = {
	.enable = pf0900_regulator_enable_regmap,
	.disable = pf0900_regulator_disable_regmap,
	.is_enabled = pf0900_regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear_range,
	.set_voltage_sel = pf0900_regulator_set_voltage_sel_regmap,
	.get_voltage_sel = pf0900_regulator_get_voltage_sel_regmap,
};

/*
 * SW1/2/3/4/5
 * SW1_DVS[1:0] SW1 DVS ramp rate setting
 * 00: 15.6mV/8usec
 * 01: 15.6mV/4usec
 * 10: 15.6mV/2usec
 * 11: 15.6mV/1usec
 */
static const unsigned int pf0900_dvs_sw_ramp_table[] = {
	1950, 3900, 7800, 15600
};

/* VAON 1.8V, 3.0V, or 3.3V */
static const int pf0900_vaon_voltages[] = {
	0, 1800000, 3000000, 3300000,
};

/*
 * SW1 0.5V to 3.3V
 * 0.5V to 1.35V (6.25mV step)
 * 1.8V to 2.5V (125mV step)
 * 2.8V to 3.3V (250mV step)
 */
static const struct linear_range pf0900_dvs_sw1_volts[] = {
	REGULATOR_LINEAR_RANGE(0,        0x00, 0x08, 0),
	REGULATOR_LINEAR_RANGE(500000,   0x09, 0x91, 6250),
	REGULATOR_LINEAR_RANGE(0,        0x92, 0x9E, 0),
	REGULATOR_LINEAR_RANGE(1500000,  0x9F, 0x9F, 0),
	REGULATOR_LINEAR_RANGE(1800000,  0xA0, 0xD8, 12500),
	REGULATOR_LINEAR_RANGE(0,        0xD9, 0xDF, 0),
	REGULATOR_LINEAR_RANGE(2800000,  0xE0, 0xF4, 25000),
	REGULATOR_LINEAR_RANGE(0,        0xF5, 0xFF, 0),
};

/*
 * SW2/3/4/5 0.3V to 3.3V
 * 0.45V to 1.35V (6.25mV step)
 * 1.8V to 2.5V (125mV step)
 * 2.8V to 3.3V (250mV step)
 */
static const struct linear_range pf0900_dvs_sw2345_volts[] = {
	REGULATOR_LINEAR_RANGE(300000,   0x00, 0x00, 0),
	REGULATOR_LINEAR_RANGE(450000,   0x01, 0x91, 6250),
	REGULATOR_LINEAR_RANGE(0,        0x92, 0x9E, 0),
	REGULATOR_LINEAR_RANGE(1500000,  0x9F, 0x9F, 0),
	REGULATOR_LINEAR_RANGE(1800000,  0xA0, 0xD8, 12500),
	REGULATOR_LINEAR_RANGE(0,        0xD9, 0xDF, 0),
	REGULATOR_LINEAR_RANGE(2800000,  0xE0, 0xF4, 25000),
	REGULATOR_LINEAR_RANGE(0,        0xF5, 0xFF, 0),
};

/*
 * LDO1
 * 0.75V to 3.3V
 */
static const struct linear_range pf0900_ldo1_volts[] = {
	REGULATOR_LINEAR_RANGE(750000,   0x00, 0x0F, 50000),
	REGULATOR_LINEAR_RANGE(1800000,  0x10, 0x1F, 100000),
};

/*
 * LDO2/3
 * 0.65V to 3.3V (50mV step)
 */
static const struct linear_range pf0900_ldo23_volts[] = {
	REGULATOR_LINEAR_RANGE(650000,   0x00, 0x0D, 50000),
	REGULATOR_LINEAR_RANGE(1400000,  0x0E, 0x0F, 100000),
	REGULATOR_LINEAR_RANGE(1800000,  0x10, 0x1F, 100000),
};

/* SW1 dvs 0.5v to 1.35v
 * SW2-5 dvs 0.3v to 1.35v
 */
static int sw_set_dvs(const struct regulator_desc *desc, struct pf0900 *pf0900,
		      struct device_node *np, char *prop, unsigned int reg,
		      unsigned int mask)
{
	uint32_t uv;
	int ret, i;

	if (!desc || !pf0900 || !np || !prop)
		return -EINVAL;

	ret = of_property_read_u32(np, prop, &uv);
	if (ret == -EINVAL) {
		dev_dbg(pf0900->dev, "Property %s not found!\n", prop);
		return 0;
	} else if (ret) {
		dev_err(pf0900->dev, "Failed to read property %s: %d\n", prop, ret);
		return ret;
	}

	for (i = 0; i < desc->n_voltages; i++) {
		ret = regulator_desc_list_voltage_linear_range(desc, i);
		if (ret < 0)
			continue;
		if (ret == uv) {
			i <<= ffs(desc->vsel_mask) - 1;
			ret = pf0900_pmic_write(pf0900, reg, i, mask);
			if (ret) {
				dev_err(pf0900->dev, "Failed to write voltage to register 0x%x: %d\n",
					reg, ret);
				return ret;
			}
			return 0;
		}
	}

	return -EINVAL;
}

static int pf0900_set_dvs_levels(struct device_node *np,
				 const struct regulator_desc *desc,
				 struct regulator_config *cfg)
{
	struct pf0900_regulator_desc *data = container_of(desc,
					struct pf0900_regulator_desc, desc);
	struct pf0900 *pf0900 = dev_get_drvdata(cfg->dev);
	const struct pf0900_dvs_config *dvs = &data->dvs;
	unsigned int reg, mask;
	int i, ret = 0;
	char *prop;

	for (i = 0; i < PF0900_DVS_LEVEL_MAX; i++) {
		switch (i) {
		case PF0900_DVS_LEVEL_RUN:
			prop = "nxp,dvs-run-microvolt";
			reg = dvs->run_reg;
			mask = dvs->run_mask;
			break;
		case PF0900_DVS_LEVEL_STANDBY:
			prop = "nxp,dvs-standby-microvolt";
			reg = dvs->standby_reg;
			mask = dvs->standby_mask;
			break;
		default:
			return -EINVAL;
		}

		ret = sw_set_dvs(desc, pf0900, np, prop, reg, mask);
		if (ret)
			break;
	}

	return ret;
}

static const struct pf0900_regulator_desc pf0900_regulators[] = {
	{
		.desc = {
			.name = "vaon",
			.of_match = of_match_ptr("VAON"),
			.regulators_node = of_match_ptr("regulators"),
			.id = PF0900_VAON,
			.ops = &pf0900_avon_regulator_ops,
			.type = REGULATOR_VOLTAGE,
			.n_voltages = PF0900_VAON_VOLTAGE_NUM,
			.volt_table = pf0900_vaon_voltages,
			.enable_reg = PF0900_REG_VAON_CFG1,
			.enable_mask = VAON_MASK,
			.enable_val = VAON_1P8V,
			.vsel_reg = PF0900_REG_VAON_CFG1,
			.vsel_mask = VAON_MASK,
			.owner = THIS_MODULE,
		},
	},
	{
		.desc = {
			.name = "sw1",
			.of_match = of_match_ptr("SW1"),
			.regulators_node = of_match_ptr("regulators"),
			.id = PF0900_SW1,
			.ops = &pf0900_dvs_sw_regulator_ops,
			.type = REGULATOR_VOLTAGE,
			.n_voltages = PF0900_SW1_VOLTAGE_NUM,
			.linear_ranges = pf0900_dvs_sw1_volts,
			.n_linear_ranges = ARRAY_SIZE(pf0900_dvs_sw1_volts),
			.vsel_reg = PF0900_REG_SW1_VRUN,
			.vsel_mask = SW_VRUN_MASK,
			.enable_reg = PF0900_REG_SW1_MODE,
			.enable_mask = SW_RUN_MODE_MASK,
			.enable_val = SW_RUN_MODE_PWM,
			.ramp_reg = PF0900_REG_SW1_CFG1,
			.ramp_mask = SW_RAMP_MASK,
			.ramp_delay_table = pf0900_dvs_sw_ramp_table,
			.n_ramp_values = ARRAY_SIZE(pf0900_dvs_sw_ramp_table),
			.owner = THIS_MODULE,
			.of_parse_cb = pf0900_set_dvs_levels,
		},
		.dvs = {
			.run_reg = PF0900_REG_SW1_VRUN,
			.run_mask = SW_VRUN_MASK,
			.standby_reg = PF0900_REG_SW1_VSTBY,
			.standby_mask = SW_STBY_MASK,
		},
	},
	{
		.desc = {
			.name = "sw2",
			.of_match = of_match_ptr("SW2"),
			.regulators_node = of_match_ptr("regulators"),
			.id = PF0900_SW2,
			.ops = &pf0900_dvs_sw_regulator_ops,
			.type = REGULATOR_VOLTAGE,
			.n_voltages = PF0900_SW2_VOLTAGE_NUM,
			.linear_ranges = pf0900_dvs_sw2345_volts,
			.n_linear_ranges = ARRAY_SIZE(pf0900_dvs_sw2345_volts),
			.vsel_reg = PF0900_REG_SW2_VRUN,
			.vsel_mask = SW_VRUN_MASK,
			.enable_reg = PF0900_REG_SW2_MODE,
			.enable_mask = SW_RUN_MODE_MASK,
			.enable_val = SW_RUN_MODE_PWM,
			.ramp_reg = PF0900_REG_SW2_CFG1,
			.ramp_mask = SW_RAMP_MASK,
			.ramp_delay_table = pf0900_dvs_sw_ramp_table,
			.n_ramp_values = ARRAY_SIZE(pf0900_dvs_sw_ramp_table),
			.owner = THIS_MODULE,
			.of_parse_cb = pf0900_set_dvs_levels,
		},
		.dvs = {
			.run_reg = PF0900_REG_SW2_VRUN,
			.run_mask = SW_VRUN_MASK,
			.standby_reg = PF0900_REG_SW2_VSTBY,
			.standby_mask = SW_STBY_MASK,
		},
	},
	{
		.desc = {
			.name = "sw3",
			.of_match = of_match_ptr("SW3"),
			.regulators_node = of_match_ptr("regulators"),
			.id = PF0900_SW3,
			.ops = &pf0900_dvs_sw_regulator_ops,
			.type = REGULATOR_VOLTAGE,
			.n_voltages = PF0900_SW3_VOLTAGE_NUM,
			.linear_ranges = pf0900_dvs_sw2345_volts,
			.n_linear_ranges = ARRAY_SIZE(pf0900_dvs_sw2345_volts),
			.vsel_reg = PF0900_REG_SW3_VRUN,
			.vsel_mask = SW_VRUN_MASK,
			.enable_reg = PF0900_REG_SW3_MODE,
			.enable_mask = SW_RUN_MODE_MASK,
			.enable_val = SW_RUN_MODE_PWM,
			.ramp_reg = PF0900_REG_SW3_CFG1,
			.ramp_mask = SW_RAMP_MASK,
			.ramp_delay_table = pf0900_dvs_sw_ramp_table,
			.n_ramp_values = ARRAY_SIZE(pf0900_dvs_sw_ramp_table),
			.owner = THIS_MODULE,
			.of_parse_cb = pf0900_set_dvs_levels,
		},
		.dvs = {
			.run_reg = PF0900_REG_SW3_VRUN,
			.run_mask = SW_VRUN_MASK,
			.standby_reg = PF0900_REG_SW3_VSTBY,
			.standby_mask = SW_STBY_MASK,
		},
	},
	{
		.desc = {
			.name = "sw4",
			.of_match = of_match_ptr("SW4"),
			.regulators_node = of_match_ptr("regulators"),
			.id = PF0900_SW5,
			.ops = &pf0900_dvs_sw_regulator_ops,
			.type = REGULATOR_VOLTAGE,
			.n_voltages = PF0900_SW4_VOLTAGE_NUM,
			.linear_ranges = pf0900_dvs_sw2345_volts,
			.n_linear_ranges = ARRAY_SIZE(pf0900_dvs_sw2345_volts),
			.vsel_reg = PF0900_REG_SW4_VRUN,
			.vsel_mask = SW_VRUN_MASK,
			.enable_reg = PF0900_REG_SW4_MODE,
			.enable_mask = SW_RUN_MODE_MASK,
			.enable_val = SW_RUN_MODE_PWM,
			.ramp_reg = PF0900_REG_SW4_CFG1,
			.ramp_mask = SW_RAMP_MASK,
			.ramp_delay_table = pf0900_dvs_sw_ramp_table,
			.n_ramp_values = ARRAY_SIZE(pf0900_dvs_sw_ramp_table),
			.owner = THIS_MODULE,
			.of_parse_cb = pf0900_set_dvs_levels,
		},
		.dvs = {
			.run_reg = PF0900_REG_SW4_VRUN,
			.run_mask = SW_VRUN_MASK,
			.standby_reg = PF0900_REG_SW4_VSTBY,
			.standby_mask = SW_STBY_MASK,
		},
	},
	{
		.desc = {
			.name = "sw5",
			.of_match = of_match_ptr("SW5"),
			.regulators_node = of_match_ptr("regulators"),
			.id = PF0900_SW5,
			.ops = &pf0900_dvs_sw_regulator_ops,
			.type = REGULATOR_VOLTAGE,
			.n_voltages = PF0900_SW5_VOLTAGE_NUM,
			.linear_ranges = pf0900_dvs_sw2345_volts,
			.n_linear_ranges = ARRAY_SIZE(pf0900_dvs_sw2345_volts),
			.vsel_reg = PF0900_REG_SW5_VRUN,
			.vsel_mask = SW_VRUN_MASK,
			.enable_reg = PF0900_REG_SW5_MODE,
			.enable_mask = SW_RUN_MODE_MASK,
			.enable_val = SW_RUN_MODE_PWM,
			.ramp_reg = PF0900_REG_SW5_CFG1,
			.ramp_mask = SW_RAMP_MASK,
			.ramp_delay_table = pf0900_dvs_sw_ramp_table,
			.n_ramp_values = ARRAY_SIZE(pf0900_dvs_sw_ramp_table),
			.owner = THIS_MODULE,
			.of_parse_cb = pf0900_set_dvs_levels,
		},
		.dvs = {
			.run_reg = PF0900_REG_SW5_VRUN,
			.run_mask = SW_VRUN_MASK,
			.standby_reg = PF0900_REG_SW5_VSTBY,
			.standby_mask = SW_STBY_MASK,
		},
	},
	{
		.desc = {
			.name = "ldo1",
			.of_match = of_match_ptr("LDO1"),
			.regulators_node = of_match_ptr("regulators"),
			.id = PF0900_LDO1,
			.ops = &pf0900_ldo_regulator_ops,
			.type = REGULATOR_VOLTAGE,
			.n_voltages = PF0900_LDO1_VOLTAGE_NUM,
			.linear_ranges = pf0900_ldo1_volts,
			.n_linear_ranges = ARRAY_SIZE(pf0900_ldo1_volts),
			.vsel_reg = PF0900_REG_LDO1_RUN,
			.vsel_mask = VLDO1_RUN_MASK,
			.enable_reg = PF0900_REG_LDO1_RUN,
			.enable_mask = LDO1_RUN_EN_MASK,
			.owner = THIS_MODULE,
		},
	},
	{
		.desc = {
			.name = "ldo2",
			.of_match = of_match_ptr("LDO2"),
			.regulators_node = of_match_ptr("regulators"),
			.id = PF0900_LDO2,
			.ops = &pf0900_ldo_regulator_ops,
			.type = REGULATOR_VOLTAGE,
			.n_voltages = PF0900_LDO2_VOLTAGE_NUM,
			.linear_ranges = pf0900_ldo23_volts,
			.n_linear_ranges = ARRAY_SIZE(pf0900_ldo23_volts),
			.vsel_reg = PF0900_REG_LDO2_RUN,
			.vsel_mask = VLDO2_RUN_MASK,
			.enable_reg = PF0900_REG_LDO2_RUN,
			.enable_mask = LDO2_RUN_EN_MASK,
			.owner = THIS_MODULE,
		},
	},
	{
		.desc = {
			.name = "ldo3",
			.of_match = of_match_ptr("LDO3"),
			.regulators_node = of_match_ptr("regulators"),
			.id = PF0900_LDO3,
			.ops = &pf0900_ldo_regulator_ops,
			.type = REGULATOR_VOLTAGE,
			.n_voltages = PF0900_LDO3_VOLTAGE_NUM,
			.linear_ranges = pf0900_ldo23_volts,
			.n_linear_ranges = ARRAY_SIZE(pf0900_ldo23_volts),
			.vsel_reg = PF0900_REG_LDO3_RUN,
			.vsel_mask = VLDO3_RUN_MASK,
			.enable_reg = PF0900_REG_LDO3_RUN,
			.enable_mask = LDO3_RUN_EN_MASK,
			.owner = THIS_MODULE,
		},
	},
};

static irqreturn_t pf0900_irq_handler(int irq, void *data)
{
	unsigned int system, status1, status2, status3;
	struct pf0900 *pf0900 = data;
	int ret;

	ret = pf0900_pmic_read(pf0900, PF0900_REG_SYSTEM_INT, &system);
	if (ret < 0) {
		dev_err(pf0900->dev, "Failed to read SYSTEM_INT(%d)\n", ret);
		return IRQ_NONE;
	}

	ret = pf0900_pmic_read(pf0900, PF0900_REG_STATUS1_INT, &status1);
	if (ret < 0) {
		dev_err(pf0900->dev, "Failed to read STATUS1_INT(%d)\n", ret);
		return IRQ_NONE;
	}

	ret = pf0900_pmic_write(pf0900, PF0900_REG_STATUS1_INT, status1, status1);
	if (ret < 0) {
		dev_err(pf0900->dev, "Failed to write STATUS1_INT(%d)\n", ret);
		return IRQ_NONE;
	}

	ret = pf0900_pmic_read(pf0900, PF0900_REG_STATUS2_INT, &status2);
	if (ret < 0) {
		dev_err(pf0900->dev, "Failed to read STATUS2_INT(%d)\n", ret);
		return IRQ_NONE;
	}

	ret = pf0900_pmic_write(pf0900, PF0900_REG_STATUS2_INT, status2, status2);
	if (ret < 0) {
		dev_err(pf0900->dev, "Failed to write STATUS2_INT(%d)\n", ret);
		return IRQ_NONE;
	}

	ret = pf0900_pmic_read(pf0900, PF0900_REG_STATUS3_INT, &status3);
	if (ret < 0) {
		dev_err(pf0900->dev, "Failed to read STATUS3_INT(%d)\n", ret);
		return IRQ_NONE;
	}

	ret = pf0900_pmic_write(pf0900, PF0900_REG_STATUS3_INT, status3, status3);
	if (ret < 0) {
		dev_err(pf0900->dev, "Failed to write STATUS3_INT(%d)\n", ret);
		return IRQ_NONE;
	}

	if (system & IRQ_EWARN)
		dev_warn(pf0900->dev, "EWARN interrupt.\n");

	if (system & IRQ_GPIO)
		dev_warn(pf0900->dev, "GPIO interrupt.\n");

	if (system & IRQ_OV)
		dev_warn(pf0900->dev, "OV interrupt.\n");

	if (system & IRQ_UV)
		dev_warn(pf0900->dev, "UV interrupt.\n");

	if (system & IRQ_ILIM)
		dev_warn(pf0900->dev, "ILIM interrupt.\n");

	if (system & IRQ_MODE)
		dev_warn(pf0900->dev, "IRQ_MODE interrupt.\n");

	if (system & status1 & IRQ_SDWN)
		dev_warn(pf0900->dev, "IRQ_SDWN interrupt.\n");

	if (system & status1 & IRQ_FREQ_RDY)
		dev_warn(pf0900->dev, "IRQ_FREQ_RDY interrupt.\n");

	if (system & status1 & IRQ_DCRC)
		dev_warn(pf0900->dev, "IRQ_DCRC interrupt.\n");

	if (system & status1 & IRQ_I2C_CRC)
		dev_warn(pf0900->dev, "IRQ_I2C_CRC interrupt.\n");

	if (system & status1 & IRQ_PWRDN)
		dev_warn(pf0900->dev, "IRQ_PWRDN interrupt.\n");

	if (system & status1 & IRQ_FSYNC_FLT)
		dev_warn(pf0900->dev, "IRQ_FSYNC_FLT interrupt.\n");

	if (system & status1 & IRQ_VIN_OV)
		dev_warn(pf0900->dev, "IRQ_VIN_OV interrupt.\n");

	if (system & status2 & IRQ_VANA_OV)
		dev_warn(pf0900->dev, "IRQ_VANA_OV interrupt.\n");

	if (system & status2 & IRQ_VDIG_OV)
		dev_warn(pf0900->dev, "IRQ_VDIG_OV interrupt.\n");

	if (system & status2 & IRQ_THERM_155)
		dev_warn(pf0900->dev, "IRQ_THERM_155 interrupt.\n");

	if (system & status2 & IRQ_THERM_140)
		dev_warn(pf0900->dev, "IRQ_THERM_140 interrupt.\n");

	if (system & status2 & IRQ_THERM_125)
		dev_warn(pf0900->dev, "IRQ_THERM_125 interrupt.\n");

	if (system & status2 & IRQ_THERM_110)
		dev_warn(pf0900->dev, "IRQ_THERM_110 interrupt.\n");

	if (system & status3 & IRQ_BAD_CMD)
		dev_warn(pf0900->dev, "IRQ_BAD_CMD interrupt.\n");

	if (system & status3 & IRQ_LBIST_DONE)
		dev_warn(pf0900->dev, "IRQ_LBIST_DONE interrupt.\n");

	if (system & status3 & IRQ_SHS)
		dev_warn(pf0900->dev, "IRQ_SHS interrupt.\n");

	return IRQ_HANDLED;
}

static int pf0900_i2c_probe(struct i2c_client *i2c)
{
	const struct pf0900_regulator_desc *regulator_desc;
	const struct pf0900_drvdata *drvdata = NULL;
	struct device_node *np = i2c->dev.of_node;
	unsigned int device_id, device_fam, i;
	struct regulator_config config = { };
	struct pf0900 *pf0900;
	int ret;

	if (!i2c->irq)
		return dev_err_probe(&i2c->dev, -EINVAL, "No IRQ configured?\n");

	pf0900 = devm_kzalloc(&i2c->dev, sizeof(struct pf0900), GFP_KERNEL);
	if (!pf0900)
		return -ENOMEM;

	drvdata = device_get_match_data(&i2c->dev);
	if (!drvdata)
		return dev_err_probe(&i2c->dev, -EINVAL, "unable to find driver data\n");

	regulator_desc = drvdata->desc;
	pf0900->drvdata = drvdata;
	pf0900->crc_en = of_property_read_bool(np, "nxp,i2c-crc-enable");
	pf0900->irq = i2c->irq;
	pf0900->dev = &i2c->dev;
	pf0900->addr = i2c->addr;

	dev_set_drvdata(&i2c->dev, pf0900);

	pf0900->regmap = devm_regmap_init_i2c(i2c,
					       &pf0900_regmap_config);
	if (IS_ERR(pf0900->regmap))
		return dev_err_probe(&i2c->dev, PTR_ERR(pf0900->regmap),
				     "regmap initialization failed\n");

	ret = pf0900_pmic_read(pf0900, PF0900_REG_DEV_ID, &device_id);
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "Read device id error\n");

	ret = pf0900_pmic_read(pf0900, PF0900_REG_DEV_FAM, &device_fam);
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "Read device fam error\n");

	/* Check your board and dts for match the right pmic */
	if (device_fam == 0x09 && (device_id & 0x1F) != 0x0 &&
	    drvdata->type == PF0900_TYPE_PF0900)
		return dev_err_probe(&i2c->dev, -EINVAL, "Device id(%x) mismatched\n",
			device_id >> 4);

	for (i = 0; i < drvdata->rcnt; i++) {
		const struct regulator_desc *desc;
		struct regulator_dev *rdev;
		const struct pf0900_regulator_desc *r;

		r = &regulator_desc[i];
		desc = &r->desc;
		config.regmap = pf0900->regmap;
		config.dev = pf0900->dev;

		rdev = devm_regulator_register(pf0900->dev, desc, &config);
		if (IS_ERR(rdev))
			return dev_err_probe(pf0900->dev, PTR_ERR(rdev),
					     "Failed to register regulator(%s)\n", desc->name);
	}

	ret = devm_request_threaded_irq(pf0900->dev, pf0900->irq, NULL,
					pf0900_irq_handler,
					(IRQF_TRIGGER_FALLING | IRQF_ONESHOT),
					"pf0900-irq", pf0900);

	if (ret != 0)
		return dev_err_probe(pf0900->dev, ret, "Failed to request IRQ: %d\n",
				     pf0900->irq);
	/*
	 * The PWRUP_M is unmasked by default. When the device enter in RUN state,
	 * it will assert the PWRUP_I interrupt and assert the INTB pin to inform
	 * the MCU that it has finished the power up sequence properly.
	 */
	ret = pf0900_pmic_write(pf0900, PF0900_REG_STATUS1_INT, IRQ_PWRUP, IRQ_PWRUP);
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "Clean PWRUP_I error\n");

	/* Unmask all interrupt except PWRUP */
	ret = pf0900_pmic_write(pf0900, PF0900_REG_STATUS1_MSK, IRQ_PWRUP,
				IRQ_SDWN | IRQ_FREQ_RDY | IRQ_DCRC |
				IRQ_I2C_CRC | IRQ_PWRUP | IRQ_PWRDN |
				IRQ_FSYNC_FLT | IRQ_VIN_OV);
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "Unmask irq error\n");

	/* Unmask all interrupt except BGMON/CLKMON */
	ret = pf0900_pmic_write(pf0900, PF0900_REG_STATUS2_MSK, IRQ_BGMON | IRQ_CLKMON,
				IRQ_VANA_OV | IRQ_VDIG_OV | IRQ_THERM_155 |
				IRQ_THERM_140 | IRQ_THERM_125 | IRQ_THERM_110);
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "Unmask irq error\n");

	ret = pf0900_pmic_write(pf0900, PF0900_REG_STATUS3_MSK, 0,
				IRQ_BAD_CMD | IRQ_LBIST_DONE | IRQ_SHS);
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "Unmask irq error\n");

	dev_info(&i2c->dev, "%s probed.\n", drvdata->type == PF0900_TYPE_PF0900 ?
		 "pf0900" : "unknown pmic");

	return 0;
}

static struct pf0900_drvdata pf0900_drvdata = {
	.desc = pf0900_regulators,
	.type = PF0900_TYPE_PF0900,
	.rcnt = ARRAY_SIZE(pf0900_regulators),
};

static const struct of_device_id pf0900_of_match[] = {
	{ .compatible = "nxp,pf0900", .data = &pf0900_drvdata},
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, pf0900_of_match);

static struct i2c_driver pf0900_i2c_driver = {
	.driver = {
		.name = "nxp-pf0900",
		.of_match_table = pf0900_of_match,
	},
	.probe = pf0900_i2c_probe,
};

module_i2c_driver(pf0900_i2c_driver);

MODULE_AUTHOR("Joy Zou <joy.zou@nxp.com>");
MODULE_DESCRIPTION("NXP PF0900 Power Management IC driver");
MODULE_LICENSE("GPL");
