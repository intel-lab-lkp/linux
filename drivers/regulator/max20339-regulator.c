// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Linaro Ltd.
 *
 * Maxim MAX20339 load switch with over voltage protection
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/types.h>

/* registers */
#define MAX20339_REVISION_ID			0x0

/* status1, int1, intmask1 share the same fields */
#define MAX20339_STATUS1			0x1
#define MAX20339_INT1				0x4
#define MAX20339_INTMASK1			0x7
#define MAX20339_OTPRDY				BIT(6)
#define MAX20339_VINVALID			BIT(5)
#define MAX20339_THMFAULT			BIT(4)
#define MAX20339_INOVFAULT			BIT(2)
#define MAX20339_INSWCLOSED			BIT(0)

/*
 * status2/3, int2/3, intmask2/3 share the same fields
 * ...2 are for load switch 1
 * ...3 are for load switch 2
 */
#define MAX20339_STATUS2			0x2
#define MAX20339_STATUS3			0x3
#define MAX20339_INT2				0x5
#define MAX20339_INT3				0x6
#define MAX20339_INTMASK2			0x8
#define MAX20339_INTMASK3			0x9
#define MAX20339_LSWxSHORTFAULT			BIT(4)
#define MAX20339_LSWxSS				BIT(3)
#define MAX20339_LSWxOVFAULT			BIT(2)
#define MAX20339_LSWxOCFAULT			BIT(1)
#define MAX20339_LSWxCLOSED			BIT(0)

#define MAX20339_SWCNTL				0xa
#define MAX20339_SWCNTL_LSW2OVEN		BIT(5)
#define MAX20339_SWCNTL_LSW2EN			BIT(4)
#define MAX20339_SWCNTL_LSW1OVEN		BIT(1)
#define MAX20339_SWCNTL_LSW1EN			BIT(0)

#define MAX20339_SWILIMDIV			0xb
#define MAX20339_SWILIMDIV_LSW2ILIMDIV		GENMASK(7, 4)
#define MAX20339_SWILIMDIV_LSW1ILIMDIV		GENMASK(3, 0)

#define MAX20339_IN_CTR				0x10
#define MAX20339_IN_CTR_INDISEN			BIT(2)
#define MAX20339_IN_CTR_INSWEN			GENMASK(1, 0)
#define MAX20339_IN_CTR_INSWEN_FORCE_OFF	0
#define MAX20339_IN_CTR_INSWEN_AUTO		1
#define MAX20339_IN_CTR_INSWEN_FORCE_ON		3

#define MAX20339_OVLOSEL			0x11
#define MAX20339_OVLOSEL_INOVLOACT		GENMASK(5, 4)
#define MAX20339_OVLOSEL_INOVLOSEL		GENMASK(1, 0)
#define MAX20339_OVLOSEL_5v85			0
#define MAX20339_OVLOSEL_9v7			1
#define MAX20339_OVLOSEL_14v5			2

#define MAX20339_COMM_CTRL			0x15
#define MAX20339_COMM_CTRL_SWRESET		BIT(0)

#define MAX20339_LAST_REGISTER			0x15


struct max20339_irq_data {
	struct device *dev;
	struct regmap *regmap;
#define MAX20339_REGULATOR_INSW 0
#define MAX20339_REGULATOR_LSW1 1
#define MAX20339_REGULATOR_LSW2 2
#define MAX20339_N_REGULATORS 3
	struct regulator_dev *rdevs[MAX20339_N_REGULATORS];
};

struct max20339_regulator {
	struct regulator_desc desc;
	int limit;
	/* for lsw only */
	u32 shunt_micro_ohm;
	unsigned int ovp_mask;
	unsigned int status_reg;
};

static const struct regmap_range max20339_registers[] = {
	regmap_reg_range(MAX20339_REVISION_ID, MAX20339_SWILIMDIV),
	regmap_reg_range(MAX20339_IN_CTR, MAX20339_OVLOSEL),
	regmap_reg_range(MAX20339_COMM_CTRL, MAX20339_COMM_CTRL),
};

static const struct regmap_range max20339_ro_registers[] = {
	regmap_reg_range(MAX20339_REVISION_ID, MAX20339_INT3),
};

static const struct regmap_access_table max20339_write_table = {
	.yes_ranges = max20339_registers,
	.n_yes_ranges = ARRAY_SIZE(max20339_registers),
	.no_ranges = max20339_ro_registers,
	.n_no_ranges = ARRAY_SIZE(max20339_ro_registers),
};

static const struct regmap_access_table max20339_rd_table = {
	.yes_ranges = max20339_registers,
	.n_yes_ranges = ARRAY_SIZE(max20339_registers),
};

static const struct regmap_range max20339_volatile_ranges[] = {
	regmap_reg_range(MAX20339_STATUS1, MAX20339_INT3),
	regmap_reg_range(MAX20339_SWCNTL, MAX20339_SWCNTL),
	regmap_reg_range(MAX20339_OVLOSEL, MAX20339_OVLOSEL),
};

static const struct regmap_access_table max20339_volatile_table = {
	.yes_ranges = max20339_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(max20339_volatile_ranges),
};

static const struct regmap_range max20339_precious_ranges[] = {
	regmap_reg_range(MAX20339_INT1, MAX20339_INT3),
};

static const struct regmap_access_table max20339_precious_table = {
	.yes_ranges = max20339_precious_ranges,
	.n_yes_ranges = ARRAY_SIZE(max20339_precious_ranges),
};

static const struct regmap_config max20339_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX20339_LAST_REGISTER,
	.wr_table = &max20339_write_table,
	.rd_table = &max20339_rd_table,
	.volatile_table = &max20339_volatile_table,
	.precious_table = &max20339_precious_table,
};


static irqreturn_t max20339_irq(int irqno, void *data)
{
	struct max20339_irq_data *max20339 = data;
	struct device *dev = max20339->dev;
	struct regmap *regmap = max20339->regmap;
	u8 status[6];
	int ret;

	ret = regmap_bulk_read(regmap, MAX20339_STATUS1, status,
			       ARRAY_SIZE(status));
	if (ret) {
		dev_err(dev, "Failed to read IRQ status: %d\n", ret);
		return IRQ_NONE;
	}

	dev_dbg(dev,
		"INT1 2 3: %#.2x %#.2x %#.2x STATUS1 2 3: %#.2x %#.2x %#.2x\n",
		status[3], status[4], status[5], status[0], status[1],
		status[2]);

	if (!status[3] && !status[4] && !status[5])
		return IRQ_NONE;

	/* overall status */
	if (status[3] & status[0] & MAX20339_THMFAULT) {
		dev_warn(dev, "Thermal fault\n");
		for (int i = 0; i < ARRAY_SIZE(max20339->rdevs); ++i)
			regulator_notifier_call_chain(max20339->rdevs[i],
						      REGULATOR_EVENT_OVER_TEMP,
						      NULL);
	}

	/* INSW status */
	if ((status[3] & MAX20339_VINVALID)
	    && !(status[0] & MAX20339_VINVALID)) {
		dev_warn(dev, "Vin over- or undervoltage\n");
		regulator_notifier_call_chain(max20339->rdevs[MAX20339_REGULATOR_INSW],
					      (REGULATOR_EVENT_UNDER_VOLTAGE_WARN
					       | REGULATOR_EVENT_OVER_VOLTAGE_WARN),
					      NULL);
	}

	if (status[3] & status[0] & MAX20339_INOVFAULT) {
		dev_warn(dev, "Over voltage on INput\n");
		regulator_notifier_call_chain(max20339->rdevs[MAX20339_REGULATOR_INSW],
					      REGULATOR_EVENT_OVER_VOLTAGE_WARN,
					      NULL);
	}

	/* LSW1 status */
	if (status[4] & status[1] & MAX20339_LSWxSHORTFAULT) {
		dev_warn(dev, "Current short on LSW1\n");
		regulator_notifier_call_chain(max20339->rdevs[MAX20339_REGULATOR_LSW1],
					      REGULATOR_EVENT_OVER_CURRENT, NULL);
	}

	if (status[4] & status[1] & MAX20339_LSWxOVFAULT) {
		dev_warn(dev, "Overvoltage on LSW1\n");
		regulator_notifier_call_chain(max20339->rdevs[MAX20339_REGULATOR_LSW1],
					      REGULATOR_EVENT_OVER_VOLTAGE_WARN, NULL);
	}

	if (status[4] & status[1] & MAX20339_LSWxOCFAULT) {
		dev_warn(dev, "Overcurrent on LSW1\n");
		regulator_notifier_call_chain(max20339->rdevs[MAX20339_REGULATOR_LSW1],
					      REGULATOR_EVENT_OVER_CURRENT, NULL);
	}

	/* LSW2 status */
	if (status[5] & status[2] & MAX20339_LSWxSHORTFAULT) {
		dev_warn(dev, "Current short on LSW2\n");
		regulator_notifier_call_chain(max20339->rdevs[MAX20339_REGULATOR_LSW2],
					      REGULATOR_EVENT_OVER_CURRENT, NULL);
	}

	if (status[5] & status[2] & MAX20339_LSWxOVFAULT) {
		dev_warn(dev, "Overvoltage on LSW2\n");
		regulator_notifier_call_chain(max20339->rdevs[MAX20339_REGULATOR_LSW2],
					      REGULATOR_EVENT_OVER_VOLTAGE_WARN, NULL);
	}

	if (status[5] & status[2] & MAX20339_LSWxOCFAULT) {
		dev_warn(dev, "Overcurrent on LSW2\n");
		regulator_notifier_call_chain(max20339->rdevs[MAX20339_REGULATOR_LSW2],
					      REGULATOR_EVENT_OVER_CURRENT, NULL);
	}

	return IRQ_HANDLED;
}

static int max20339_insw_is_enabled(struct regulator_dev *rdev)
{
	unsigned int val;
	int ret;
	struct device *dev = rdev_get_dev(rdev);

	ret = regmap_read(rdev_get_regmap(rdev), MAX20339_STATUS1, &val);
	if (ret) {
		dev_err(dev, "error reading STATUS1: %d\n", ret);
		return ret;
	}

	dev_dbg(dev, "%s: %s: %c\n", __func__, rdev->desc->name,
		"ny"[FIELD_GET(MAX20339_INSWCLOSED, val)]);

	return FIELD_GET(MAX20339_INSWCLOSED, val) == 1;
}

static int max20339_set_ovlo_helper(struct regulator_dev *rdev,
				    unsigned int val)
{
	struct regmap *regmap = rdev_get_regmap(rdev);
	struct device *dev = rdev_get_dev(rdev);
	int ret;

	/*
	 * Since changes take effect only when the switch is open, we try our
	 * best to not fail: apply the operation if open, and otherwise return
	 * success if old and new value are the same.
	 * The alternative would be to turn the regulator off in between, which
	 * doesn't seem like a great idea, as it might break consumers'
	 * expectations.
	 */
	ret = rdev->desc->ops->is_enabled(rdev);
	if (ret < 0)
		return ret;

	if (ret) {
		unsigned int old;

		ret = regmap_read(regmap, MAX20339_OVLOSEL, &old);
		if (ret) {
			dev_err(dev, "error reading OVLOSEL: %d\n", ret);
			return ret;
		}

		if (FIELD_GET(MAX20339_OVLOSEL_INOVLOACT, old)
		    != FIELD_GET(MAX20339_OVLOSEL_INOVLOSEL, val)) {
			dev_warn(dev,
				 "can not change OVP %ld -> %ld while regulator is enabled\n",
				 FIELD_GET(MAX20339_OVLOSEL_INOVLOACT, old),
				 FIELD_GET(MAX20339_OVLOSEL_INOVLOSEL, val));
			return -EBUSY;
		}
	}


	ret = regmap_update_bits(regmap, MAX20339_OVLOSEL,
				 MAX20339_OVLOSEL_INOVLOSEL, val);
	if (ret)
		dev_err(dev, "error updating OVLOSEL: %d\n", ret);

	return ret;
}

static int max20339_insw_set_ovp(struct regulator_dev *rdev, int lim_uV,
				 int severity, bool enable)
{
	struct max20339_regulator *data = rdev_get_drvdata(rdev);
	struct device *dev = rdev_get_dev(rdev);
	unsigned int val;

	dev_dbg(dev, "%s: %s: lim: %duV, sev: %d, en: %c\n", __func__,
		rdev->desc->name, lim_uV, severity, "ny"[!!enable]);

	/* max20339 does not provide warnings, only protection */
	if (severity != REGULATOR_SEVERITY_PROT) {
		dev_err(dev, "unsupported over voltage protection level\n");
		return -EINVAL;
	}

	/*
	 * max20339 can not disable OVP.
	 * If limit is specified but enable is not set, then remember the value
	 * and use it later when enabling OVP, because the regulator API allows
	 * enabling a previously set limit by passing 0 as limit.
	 */
	if (lim_uV)
		data->limit = lim_uV;

	if (!enable) {
		/*
		 * since there is no way to disable OVP, just set the limit to
		 * the maximum
		 */
		val = FIELD_PREP(MAX20339_OVLOSEL_INOVLOSEL,
				 MAX20339_OVLOSEL_14v5);
	} else {
		/*
		 * protection supports three limits: 5.85V, 9.7V, 14.5V
		 * we always use the lower bound to avoid supplying a voltage
		 * that is higher than requested
		 */
		if (data->limit < 5850000)
			return -EINVAL;

		if (data->limit < 9700000)
			val = FIELD_PREP(MAX20339_OVLOSEL_INOVLOSEL,
					 MAX20339_OVLOSEL_5v85);
		else if (data->limit < 14500000)
			val = FIELD_PREP(MAX20339_OVLOSEL_INOVLOSEL,
					 MAX20339_OVLOSEL_9v7);
		else
			val = FIELD_PREP(MAX20339_OVLOSEL_INOVLOSEL,
					 MAX20339_OVLOSEL_14v5);
	}

	return max20339_set_ovlo_helper(rdev, val);
}

static int max20339_set_voltage_sel(struct regulator_dev *rdev,
				    unsigned int sel)
{
	return max20339_set_ovlo_helper(rdev,
					FIELD_PREP(MAX20339_OVLOSEL_INOVLOSEL,
						   sel));
}

static int max20339_get_voltage_sel(struct regulator_dev *rdev)
{
	unsigned int val;
	int ret;

	ret = regmap_read(rdev_get_regmap(rdev), MAX20339_OVLOSEL, &val);
	if (ret)
		return ret;

	return FIELD_GET(MAX20339_OVLOSEL_INOVLOACT, val);
}

static int max20339_insw_get_error_flags(struct regulator_dev *rdev,
					 unsigned int *flags)
{
	unsigned int val;
	int ret;

	ret = regmap_read(rdev_get_regmap(rdev), MAX20339_STATUS1, &val);
	if (ret) {
		dev_err(rdev_get_dev(rdev),
			"Failed to read MAX20339_STATUS1: %d\n", ret);
		return ret;
	}

	*flags = 0;

	if (!(val & MAX20339_VINVALID)) {
		*flags |= REGULATOR_ERROR_UNDER_VOLTAGE_WARN;
		*flags |= REGULATOR_ERROR_OVER_VOLTAGE_WARN;
	}

	if (val & MAX20339_THMFAULT)
		*flags |= REGULATOR_ERROR_OVER_TEMP;

	if (val & MAX20339_INOVFAULT)
		*flags |= REGULATOR_ERROR_OVER_VOLTAGE_WARN;

	return 0;
}

static const struct regulator_ops max20339_insw_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = max20339_insw_is_enabled,

	.set_over_voltage_protection = max20339_insw_set_ovp,

	.set_active_discharge = regulator_set_active_discharge_regmap,

	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.set_voltage_sel = max20339_set_voltage_sel,
	.get_voltage_sel = max20339_get_voltage_sel,

	.get_error_flags = max20339_insw_get_error_flags,
};

static const unsigned int max20339_insw_volt_table[] = {
	5850000, 9700000, 14500000
};

static int max20339_lsw_is_enabled(struct regulator_dev *rdev)
{
	struct max20339_regulator *data = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret;
	struct device *dev = rdev_get_dev(rdev);

	ret = regmap_read(rdev_get_regmap(rdev), data->status_reg, &val);
	if (ret) {
		dev_err(dev, "error reading STATUS%d: %d\n",
			data->status_reg, ret);
		return ret;
	}

	dev_dbg(dev, "%s: %s: %c\n", __func__, rdev->desc->name,
		"ny"[FIELD_GET(MAX20339_LSWxCLOSED, val)]);

	return FIELD_GET(MAX20339_LSWxCLOSED, val) == 1;
}

static int max20339_lsw_set_ocp(struct regulator_dev *rdev, int lim_uA,
				int severity, bool enable)
{
	struct device *dev = rdev_get_dev(rdev);
	struct max20339_regulator *data = rdev_get_drvdata(rdev);
	struct regmap *regmap;
	int ret;
	unsigned int ilimdiv;

	dev_dbg(dev, "%s, %s: lim: %duA, sev: %d, en: %c\n", __func__,
		rdev->desc->name, lim_uA, severity, "ny"[!!enable]);

	/* max20339 does not provide warnings, only protection */
	if (severity != REGULATOR_SEVERITY_PROT) {
		dev_err(dev, "unsupported over current protection level\n");
		return -EINVAL;
	}

	regmap = rdev_get_regmap(rdev);
	/*
	 * max20339 can not set a limit without also enforcing it.
	 * If limit is specified but enable is not set, then remember the value
	 * and use it later when enabling OCP, because the regulator API allows
	 * enabling a previously set limit by passing 0 as limit.
	 */
	if (lim_uA)
		data->limit = lim_uA;

	if (!enable) {
		ilimdiv = 0;
	} else {
		u64 tmp;
		u32 rem;

		/*
		 * Changes take effect either when the switch is open, or if it
		 * is closed and limiting is enabled already, i.e. we can not
		 * set a limit on a closed switch that currently has no limit.
		 * We deny changing if the limit can not take effect.
		 */
		ret = rdev->desc->ops->is_enabled(rdev);
		if (ret < 0)
			return ret;
		if (ret) {
			/*
			 * currently enabled, only continue if a limit is
			 * also active
			 */
			ret = regmap_read(regmap,
					  rdev->desc->csel_reg, &ilimdiv);
			if (ret) {
				dev_err(dev,
					"error reading current limit: %d\n",
					ret);
				return ret;
			}

			/* FIELD_GET() needs a constant mask */
			ilimdiv &= rdev->desc->csel_mask;
			if (!ilimdiv) {
				dev_warn(dev,
					 "can not enable current limit while regulator is enabled\n");
				return -EBUSY;
			}
		}

		/*
		 * The resulting current limit is:
		 *     ILim = 175 / shunt_ohm / ILimDiv
		 * Therefore, ILimDiv can be calculated as:
		 *     ILimDiv = 175 / shunt_ohm / ILim = 175 / (shunt_ohm * ILim)
		 * We also need to round the divider up to avoid supplying a
		 * higher current than requested.
		 */
		tmp = div_u64(mul_u32_u32(data->shunt_micro_ohm, data->limit),
			      1000000);
		ilimdiv = div_u64_rem(175000000LLU, tmp, &rem);
		if (rem)
			++ilimdiv;
		/* FIELD_PREP() needs a constant mask */
		ilimdiv <<= __bf_shf(rdev->desc->csel_mask);
		if (ilimdiv < 1 || ((ilimdiv & rdev->desc->csel_mask)
				    != ilimdiv)) {
			dev_err(dev,
				"over current limit %duA not achievable\n",
				data->limit);
			return -EINVAL;
		}
	}

	ret = regmap_update_bits(regmap, rdev->desc->csel_reg,
				 rdev->desc->csel_mask, ilimdiv);
	if (ret)
		dev_err(dev, "error updating current limit: %d\n", ret);

	return ret;
}

static int max20339_lsw_set_ovp(struct regulator_dev *rdev, int lim_uV,
				int severity, bool enable)
{
	struct device *dev = rdev_get_dev(rdev);
	struct max20339_regulator *data = rdev_get_drvdata(rdev);
	int ret;

	dev_dbg(dev, "%s: %s: lim: %duV, sev: %d, en: %c\n", __func__,
		rdev->desc->name, lim_uV, severity, "ny"[!!enable]);

	/* max20339 does not provide warnings, only protection */
	if (severity != REGULATOR_SEVERITY_PROT) {
		dev_err(dev, "unsupported over voltage protection level\n");
		return -EINVAL;
	}

	/*
	 * on the load switches, the limit is not changeable and can only be
	 * enabled or disabled
	 */
	if (lim_uV) {
		dev_err(dev,
			"changing over voltage protection limit not supported\n");
		return -EINVAL;
	}

	ret = regmap_update_bits(rdev_get_regmap(rdev), rdev->desc->enable_reg,
				 data->ovp_mask,
				 /* FIELD_PREP() needs a constant mask */
				 !!enable << __bf_shf(data->ovp_mask));
	if (ret)
		dev_err(dev, "error changing OVP: %d\n", ret);

	return ret;

}

static int max20339_lsw_get_error_flags(struct regulator_dev *rdev,
					unsigned int *flags)
{
	struct max20339_regulator *data = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret;

	ret = regmap_read(rdev_get_regmap(rdev), data->status_reg, &val);
	if (ret) {
		dev_err(rdev_get_dev(rdev),
			"Failed to read MAX20339_STATUS%d: %d\n",
			data->status_reg, ret);
		return ret;
	}

	*flags = 0;

	if (val & MAX20339_LSWxSHORTFAULT)
		*flags |= REGULATOR_ERROR_OVER_CURRENT;

	if (val & MAX20339_LSWxOVFAULT)
		*flags |= REGULATOR_ERROR_OVER_VOLTAGE_WARN;

	if (val & MAX20339_LSWxOCFAULT)
		*flags |= REGULATOR_ERROR_OVER_CURRENT;

	return 0;
}

static int max20339_lsw_dt_parse(struct device_node *np,
				 const struct regulator_desc *desc,
				 struct regulator_config *cfg)
{
	struct max20339_regulator *data = cfg->driver_data;

	/* we turn missing properties into a fatal issue during probe() */
	return of_property_read_u32(np, "shunt-resistor-micro-ohms",
				    &data->shunt_micro_ohm);
}

static const struct regulator_ops max20339_lsw_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = max20339_lsw_is_enabled,

	.set_over_current_protection = max20339_lsw_set_ocp,
	.set_over_voltage_protection = max20339_lsw_set_ovp,

	.get_error_flags = max20339_lsw_get_error_flags,
};

#define MAX20339_LSW_DESC(_sfx, _enable_mask, _csel_mask, _ovp_mask, _status_reg) { \
	.desc = {                                                 \
		.name = "max20339-" _sfx,                         \
		\
		.ops = &max20339_lsw_ops,                         \
		.type = REGULATOR_VOLTAGE,                        \
		.supply_name = _sfx,                              \
		\
		.enable_reg = MAX20339_SWCNTL,                    \
		.enable_mask = _enable_mask,                      \
		\
		.csel_reg = MAX20339_SWILIMDIV,                   \
		.csel_mask = _csel_mask,                          \
		\
		.regulators_node = of_match_ptr("regulators"),    \
		.of_match = of_match_ptr(_sfx),                   \
		.of_parse_cb = max20339_lsw_dt_parse,             \
		\
		.enable_time = 11000 + 1100,                      \
		.off_on_delay = 13,                               \
		.poll_enabled_time = 2420,                        \
		\
		.owner = THIS_MODULE,                             \
	},                                                        \
	.ovp_mask = _ovp_mask,                                    \
	.status_reg = _status_reg,                                \
}


static struct max20339_regulator max20339_regulators[MAX20339_N_REGULATORS] = {
	[MAX20339_REGULATOR_INSW] = {
		.desc = {
			.name = "max20339-insw",

			.ops = &max20339_insw_ops,
			.type = REGULATOR_VOLTAGE,
			.supply_name = "insw",

			.volt_table = max20339_insw_volt_table,
			.n_voltages = ARRAY_SIZE(max20339_insw_volt_table),

			.enable_reg = MAX20339_IN_CTR,
			.enable_mask = MAX20339_IN_CTR_INSWEN,
			.enable_val = FIELD_PREP_CONST(MAX20339_IN_CTR_INSWEN,
						  MAX20339_IN_CTR_INSWEN_AUTO),

			.active_discharge_reg = MAX20339_IN_CTR,
			.active_discharge_mask = MAX20339_IN_CTR_INDISEN,
			.active_discharge_on = MAX20339_IN_CTR_INDISEN,
			.active_discharge_off = 0,

			.regulators_node = of_match_ptr("regulators"),
			.of_match = of_match_ptr("insw"),

			.enable_time = 15000,
			.off_on_delay = 1,
			.poll_enabled_time = 3000,

			.owner = THIS_MODULE,
		}
	},
	[MAX20339_REGULATOR_LSW1] = MAX20339_LSW_DESC("lsw1",
						MAX20339_SWCNTL_LSW1EN,
						MAX20339_SWILIMDIV_LSW1ILIMDIV,
						MAX20339_SWCNTL_LSW1OVEN,
						MAX20339_STATUS2),
	[MAX20339_REGULATOR_LSW2] = MAX20339_LSW_DESC("lsw2",
						MAX20339_SWCNTL_LSW2EN,
						MAX20339_SWILIMDIV_LSW2ILIMDIV,
						MAX20339_SWCNTL_LSW2OVEN,
						MAX20339_STATUS3),
};

static int max20339_setup_irq(struct i2c_client *client,
			      struct regmap *regmap,
			      struct regulator_dev *rdevs[])
{
	u8 enabled_irqs[3];
	struct max20339_irq_data *max20339;
	int ret;
	unsigned long irq_flags;

	/* the IRQ is optional */
	if (!client->irq) {
		enabled_irqs[0] = enabled_irqs[1] = enabled_irqs[2] = 0;
	} else {
		enabled_irqs[0] = (MAX20339_VINVALID | MAX20339_THMFAULT
				   | MAX20339_INOVFAULT);
		enabled_irqs[1] = (MAX20339_LSWxSHORTFAULT
				   | MAX20339_LSWxOVFAULT
				   | MAX20339_LSWxOCFAULT);
		enabled_irqs[2] = (MAX20339_LSWxSHORTFAULT
				   | MAX20339_LSWxOVFAULT
				   | MAX20339_LSWxOCFAULT);

		max20339 = devm_kzalloc(&client->dev, sizeof(*max20339),
					GFP_KERNEL);
		if (!max20339)
			return -ENOMEM;
	}

	ret = regmap_bulk_write(regmap, MAX20339_INTMASK1,
				enabled_irqs, ARRAY_SIZE(enabled_irqs));
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "error configuring INTMASK1..3\n");

	if (!client->irq)
		return 0;

	max20339->dev = &client->dev;
	max20339->regmap = regmap;
	memcpy(max20339->rdevs, rdevs, sizeof(max20339->rdevs));

	irq_flags = IRQF_ONESHOT | IRQF_SHARED;
	irq_flags |= irqd_get_trigger_type(irq_get_irq_data(client->irq));

	ret = devm_request_threaded_irq(&client->dev, client->irq,
					NULL, max20339_irq, irq_flags,
					"max20339", max20339);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "IRQ setup failed\n");

	return 0;
}

#if IS_ENABLED(CONFIG_GPIO_REGMAP)
static int max20339_gpio_regmap_xlate(struct gpio_regmap *const gpio,
				      unsigned int base, unsigned int offset,
				      unsigned int *const reg,
				      unsigned int *const mask)
{
	if (offset != 0)
		return -EINVAL;

	*reg = base;
	*mask = MAX20339_VINVALID;
	return 0;
}

static int max20339_setup_gpio(struct i2c_client *client,
			       struct regmap *regmap)
{
	struct fwnode_handle *fwnode;
	static const char * const names[] = { "vin" };
	int ret;

	fwnode = gpiochip_node_get_first(&client->dev);
	if (!fwnode) {
		dev_info(&client->dev, "Skipping gpio chip initialization\n");
		return 0;
	}

	ret = PTR_ERR_OR_ZERO(devm_gpio_regmap_register(&client->dev,
			&(struct gpio_regmap_config) {
				.parent = &client->dev,
				.regmap = regmap,
				.fwnode = fwnode,
				.ngpio = ARRAY_SIZE(names),
				.names = names,
				.reg_dat_base = MAX20339_STATUS1,
				.reg_mask_xlate = max20339_gpio_regmap_xlate
			}));
	fwnode_handle_put(fwnode);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to initialize gpio chip\n");

	return 0;
}
#else /* CONFIG_GPIO_REGMAP */
static int max20339_setup_gpio(struct i2c_client *client,
			       struct regmap *regmap)
{
	return 0;
}
#endif /* CONFIG_GPIO_REGMAP */

static int max20339_probe(struct i2c_client *client)
{
	int ret;
	struct regmap *regmap;
	struct regulator_config config = {};
	struct regulator_dev *rdev;
	struct regulator_dev *rdevs[MAX20339_N_REGULATORS];

	/*
	 * either "dig-supply" is needed, or alternatively "in-supply" will
	 * supply power
	 */
	ret = devm_regulator_get_enable_optional(&client->dev, "dig");
	if (ret) {
		if (ret == -ENODEV)
			ret = devm_regulator_get_enable_optional(&client->dev,
								 "insw");
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to get regulator");
	}

	regmap = devm_regmap_init_i2c(client, &max20339_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err_probe(&client->dev, PTR_ERR(regmap),
			      "regmap init failed\n");
		return PTR_ERR(regmap);
	}

	for (int i = 0; i < ARRAY_SIZE(max20339_regulators); ++i) {
		config.dev = &client->dev;
		config.regmap = regmap;
		config.driver_data = &max20339_regulators[i];

		rdev = devm_regulator_register(config.dev,
					       &max20339_regulators[i].desc,
					       &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&client->dev, PTR_ERR(rdev),
					     "failed to register MAX20339 regulator %s\n",
					     max20339_regulators[i].desc.name);

		/*
		 * For the LSWs, we really need to know the shunts' values
		 * (from DT). Fail if missing.
		 */
		if (max20339_regulators[i].desc.csel_mask
		    && !max20339_regulators[i].shunt_micro_ohm)
			return dev_err_probe(&client->dev, -EINVAL,
					     "property 'shunt-resistor-micro-ohms' not found\n");

		rdevs[i] = rdev;

		dev_info(&client->dev, "registered MAX20339 regulator %s\n",
			 max20339_regulators[i].desc.name);
	}

	ret = max20339_setup_irq(client, regmap, rdevs);
	if (ret < 0)
		return ret;

	return max20339_setup_gpio(client, regmap);
}

static const struct i2c_device_id max20339_i2c_id[] = {
	{ "max20339", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max20339_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id max20339_of_id[] = {
	{ .compatible = "maxim,max20339", },
	{},
};
MODULE_DEVICE_TABLE(of, max20339_of_id);
#endif

static struct i2c_driver max20339_i2c_driver = {
	.driver = {
		.name = "max20339",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = of_match_ptr(max20339_of_id),
	},
	.probe = max20339_probe,
	.id_table = max20339_i2c_id,
};
module_i2c_driver(max20339_i2c_driver);

MODULE_AUTHOR("André Draszik <andre.draszik@linaro.org>");
MODULE_DESCRIPTION("Maxim MAX20339 OVP and load switch");
MODULE_LICENSE("GPL");
