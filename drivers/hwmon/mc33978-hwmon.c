// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
/*
 * MC33978/MC34978 Hardware Monitor Driver
 *
 * CRITICAL HARDWARE BEHAVIOR - THERMAL (tLIM):
 * When the thermal limit (>155°C) is reached, the IC autonomously
 * reduces the continuous wetting current (CWET) to 2.0 mA to prevent
 * thermal destruction. This throttling persists until the silicon cools
 * down below 140°C (15°C hysteresis).
 *
 * WARNING FOR PINCTRL/GPIO CONSUMERS:
 * During an active tLIM fault, the switch state detection becomes
 * inherently unreliable. A throttled wetting current of 2.0 mA may
 * be insufficient to break through the oxide layer of mechanical
 * contacts in the field, leading to false-open GPIO readings.
 *
 * VOLTAGE DEGRADATION WARNING (VBATP):
 * While the hard Undervoltage Lockout (UVLO) asserts strictly at 4.5V,
 * the silicon operates with degraded parametrics whenever the supply
 * drops below 6.0V. System designers must be aware that analog routing
 * (AMUX) and switch detection logic may behave non-deterministically
 * before the actual UV alarm triggers.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <linux/mfd/mc33978.h>

/* Thermal Warning threshold (~120C) */
#define MC33978_TEMP_WARN_MC		120000

/* Thermal Limit / tLIM (>155C) - Hardware enters CWET throttling */
#define MC33978_TEMP_CRIT_MC		155000

/* Hysteresis for tLIM recovery (Silicon must cool to <140C) */
#define MC33978_TEMP_HYST_MC		15000

/* VBATP (in0) IC Level thresholds */
#define MC33978_VBATP_OV_MV		36000 /* Overvoltage limit */
#define MC33978_VBATP_FUNC_MV		28000 /* Functional/Normal boundary */
#define MC33978_VBATP_DEGRADED_MV	6000 /* Degraded parametrics start */
#define MC33978_VBATP_UVLO_MV		4500 /* UV Rising Threshold max */

/* VDDQ (in1) Logic Supply thresholds */
#define MC33978_VDDQ_MAX_MV		5250 /* Operating Condition max */
#define MC33978_VDDQ_MIN_MV		3000 /* Operating Condition min */
#define MC33978_VDDQ_UV_MV		2800 /* UV Falling Threshold max */

enum mc33978_hwmon_in_channels {
	MC33978_IN_VBATP,
	MC33978_IN_VDDQ,
};

struct mc33978_hwmon_priv {
	struct device *dev;
	struct device *hwmon_dev;
	struct regmap *map;
	int fault_irq;
	u32 last_faults;
};

static int mc33978_hwmon_read_fault(struct mc33978_hwmon_priv *priv,
				    u32 *faults)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->map, MC33978_REG_FAULT, &val);
	if (ret)
		return ret;

	*faults = val;

	return 0;
}

static void mc33978_hwmon_report_faults(struct mc33978_hwmon_priv *priv,
					u32 new_faults)
{
	/*
	 * Log only newly asserted critical faults to prevent kernel log spam
	 * during persistent hardware fault conditions.
	 * dev_*_ratelimited provides an additional safety net against noisy IRQs.
	 */
	if (!new_faults)
		return;

	if (new_faults & MC33978_FAULT_OT)
		dev_crit_ratelimited(priv->dev, "Over-temperature fault detected!\n");

	if (new_faults & MC33978_FAULT_OV)
		dev_crit_ratelimited(priv->dev, "Over-voltage fault detected!\n");

	if (new_faults & MC33978_FAULT_UV)
		dev_err_ratelimited(priv->dev, "Under-voltage fault detected!\n");
}

static irqreturn_t mc33978_hwmon_fault_irq(int irq, void *data)
{
	struct mc33978_hwmon_priv *priv = data;
	u32 faults, new_faults, changed_faults;
	int ret;

	ret = mc33978_hwmon_read_fault(priv, &faults);
	if (ret) {
		dev_err_ratelimited(priv->dev, "Failed to read fault register: %pe\n",
				    ERR_PTR(ret));
		return IRQ_NONE;
	}

	changed_faults = faults ^ priv->last_faults;
	if (!changed_faults)
		return IRQ_HANDLED;

	new_faults = faults & ~priv->last_faults;
	if (new_faults)
		mc33978_hwmon_report_faults(priv, new_faults);

	priv->last_faults = faults;

	if (changed_faults & MC33978_FAULT_UV)
		hwmon_notify_event(priv->hwmon_dev, hwmon_in,
				   hwmon_in_lcrit_alarm, MC33978_IN_VBATP);

	if (changed_faults & MC33978_FAULT_OV)
		hwmon_notify_event(priv->hwmon_dev, hwmon_in,
				   hwmon_in_crit_alarm, MC33978_IN_VBATP);

	if (changed_faults & MC33978_FAULT_TEMP_WARN)
		hwmon_notify_event(priv->hwmon_dev, hwmon_temp,
				   hwmon_temp_max_alarm, 0);

	if (changed_faults & MC33978_FAULT_OT)
		hwmon_notify_event(priv->hwmon_dev, hwmon_temp,
				   hwmon_temp_crit_alarm, 0);

	/* Push a chip-level alarm on any hardware status change */
	hwmon_notify_event(priv->hwmon_dev, hwmon_chip,
			   hwmon_chip_alarms, 0);

	return IRQ_HANDLED;
}

static umode_t mc33978_hwmon_is_visible(const void *data,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	switch (type) {
	case hwmon_chip:
		if (attr == hwmon_chip_alarms)
			return 0444;
		break;

	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_max:
		case hwmon_temp_crit:
		case hwmon_temp_crit_hyst:
		case hwmon_temp_max_alarm:
		case hwmon_temp_crit_alarm:
			return 0444;
		default:
			break;
		}
		break;

	case hwmon_in:
		switch (attr) {
		case hwmon_in_label:
		case hwmon_in_max:
		case hwmon_in_min:
		case hwmon_in_lcrit:
			return 0444;
		case hwmon_in_crit:
			if (channel == MC33978_IN_VBATP)
				return 0444;
			break;
		case hwmon_in_crit_alarm:
		case hwmon_in_lcrit_alarm:
			if (channel == MC33978_IN_VBATP)
				return 0444;
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

static int mc33978_hwmon_read(struct device *dev,
			      enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct mc33978_hwmon_priv *priv = dev_get_drvdata(dev);
	u32 faults;
	int ret;

	switch (type) {
	case hwmon_in:
		if (channel == MC33978_IN_VBATP) {
			switch (attr) {
			case hwmon_in_crit:
				*val = MC33978_VBATP_OV_MV;
				return 0;
			case hwmon_in_max:
				*val = MC33978_VBATP_FUNC_MV;
				return 0;
			case hwmon_in_min:
				*val = MC33978_VBATP_DEGRADED_MV;
				return 0;
			case hwmon_in_lcrit:
				*val = MC33978_VBATP_UVLO_MV;
				return 0;
			default:
				break;
			}
		} else if (channel == MC33978_IN_VDDQ) {
			switch (attr) {
			case hwmon_in_max:
				*val = MC33978_VDDQ_MAX_MV;
				return 0;
			case hwmon_in_min:
				*val = MC33978_VDDQ_MIN_MV;
				return 0;
			case hwmon_in_lcrit:
				*val = MC33978_VDDQ_UV_MV;
				return 0;
			default:
				break;
			}
		}
		break;

	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_max:
			*val = MC33978_TEMP_WARN_MC;
			return 0;
		case hwmon_temp_crit:
			*val = MC33978_TEMP_CRIT_MC;
			return 0;
		case hwmon_temp_crit_hyst:
			*val = MC33978_TEMP_CRIT_MC - MC33978_TEMP_HYST_MC;
			return 0;
		default:
			break;
		}
		break;

	default:
		break;
	}

	/* 2. Dynamic alarms (read hardware flags) */
	ret = mc33978_hwmon_read_fault(priv, &faults);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_chip:
		if (attr == hwmon_chip_alarms) {
			*val = faults;
			return 0;
		}
		break;

	case hwmon_in:
		if (channel == MC33978_IN_VBATP) {
			switch (attr) {
			case hwmon_in_crit_alarm:
				*val = !!(faults & MC33978_FAULT_OV);
				return 0;
			case hwmon_in_lcrit_alarm:
				*val = !!(faults & MC33978_FAULT_UV);
				return 0;
			default:
				*val = 0;
				return 0;
			}
		}
		/* VDDQ has no dedicated hardware fault flags */
		*val = 0;
		return 0;

	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_max_alarm:
			*val = !!(faults & MC33978_FAULT_TEMP_WARN);
			return 0;
		case hwmon_temp_crit_alarm:
			*val = !!(faults & MC33978_FAULT_OT);
			return 0;
		default:
			break;
		}
		break;

	default:
		return -EOPNOTSUPP;
	}

	return -EOPNOTSUPP;
}

static int mc33978_hwmon_read_string(struct device *dev,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel, const char **str)
{
	/* Only in_label is supported for string reads */
	if (type != hwmon_in || attr != hwmon_in_label)
		return -EOPNOTSUPP;

	switch (channel) {
	case MC33978_IN_VBATP:
		*str = "VBATP";
		return 0;
	case MC33978_IN_VDDQ:
		*str = "VDDQ";
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct hwmon_channel_info * const mc33978_hwmon_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_ALARMS),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_MAX | HWMON_T_CRIT | HWMON_T_CRIT_HYST |
			   HWMON_T_MAX_ALARM | HWMON_T_CRIT_ALARM),
	HWMON_CHANNEL_INFO(in,
			   /* Index 0: MC33978_IN_VBATP */
			   HWMON_I_LABEL | HWMON_I_CRIT | HWMON_I_MAX |
			   HWMON_I_MIN | HWMON_I_LCRIT |
			   HWMON_I_CRIT_ALARM | HWMON_I_LCRIT_ALARM,

			   /* Index 1: MC33978_IN_VDDQ */
			   HWMON_I_LABEL | HWMON_I_MAX | HWMON_I_MIN |
			   HWMON_I_LCRIT),
	NULL
};

static const struct hwmon_ops mc33978_hwmon_ops = {
	.is_visible = mc33978_hwmon_is_visible,
	.read_string = mc33978_hwmon_read_string,
	.read = mc33978_hwmon_read,
};

static const struct hwmon_chip_info mc33978_hwmon_chip_info = {
	.ops = &mc33978_hwmon_ops,
	.info = mc33978_hwmon_info,
};

static int mc33978_hwmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mc33978_hwmon_priv *priv;
	struct device *hwmon_dev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	priv->map = dev_get_regmap(dev->parent, NULL);
	if (!priv->map)
		return dev_err_probe(dev, -ENODEV, "failed to get regmap\n");

	platform_set_drvdata(pdev, priv);

	priv->fault_irq = platform_get_irq(pdev, 0);
	if (priv->fault_irq < 0)
		return priv->fault_irq;

	ret = mc33978_hwmon_read_fault(priv, &priv->last_faults);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read initial faults\n");

	if (priv->last_faults & MC33978_FAULT_CRITICAL)
		mc33978_hwmon_report_faults(priv, priv->last_faults);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "mc33978", priv,
							 &mc33978_hwmon_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return dev_err_probe(dev, PTR_ERR(hwmon_dev),
				     "failed to register hwmon device\n");

	priv->hwmon_dev = hwmon_dev;

	ret = devm_request_threaded_irq(dev, priv->fault_irq, NULL,
					mc33978_hwmon_fault_irq, IRQF_ONESHOT,
					dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request fault IRQ\n");

	return 0;
}

static const struct platform_device_id mc33978_hwmon_id[] = {
	{ "mc33978-hwmon", },
	{ "mc34978-hwmon", },
	{ }
};
MODULE_DEVICE_TABLE(platform, mc33978_hwmon_id);

static struct platform_driver mc33978_hwmon_driver = {
	.driver = {
		.name = "mc33978-hwmon",
	},
	.probe = mc33978_hwmon_probe,
	.id_table = mc33978_hwmon_id,
};
module_platform_driver(mc33978_hwmon_driver);

MODULE_AUTHOR("Oleksij Rempel <kernel@pengutronix.de>");
MODULE_DESCRIPTION("NXP MC33978/MC34978 Hardware Monitor Driver");
MODULE_LICENSE("GPL");
