// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
/*
 * MC33978/MC34978 Hardware Monitor Driver
 *
 * Fault handling model:
 *
 * The FAULT register is clear-on-read for most bits, but persistent fault
 * conditions remain asserted. The MFD core only harvests the aggregate
 * FAULT_STAT indication from SPI responses and dispatches the hwmon child
 * IRQ on that basis. Because a persistent fault can keep FAULT_STAT asserted,
 * secondary fault assertions and fault clear events may not generate a fresh
 * interrupt edge visible to the hwmon child.
 *
 * To provide stable hwmon alarm state, this driver:
 * - caches only hwmon-relevant alarm bits
 * - serializes FAULT register reads with cache updates
 * - polls while any alarm remains active to detect secondary alarms and
 *   clearing edges
 *
 * Raw integrity bits such as SPI_ERROR and HASH are logged, but are not
 * exported through hwmon alarm attributes.
 *
 * Fault Register Access Paths:
 *
 * 1. IRQ Handler (mc33978_hwmon_fault_irq):
 *    - Triggered by rising edge on any fault assertion
 *    - Core driver harvests FAULT_STAT from SPI responses and dispatches
 *      nested IRQ to this handler
 *    - Reads Fault register, updates cache, reports new faults
 *
 * 2. Polling Worker (mc33978_hwmon_poll_work):
 *    - Runs at 1Hz while any alarm remains active
 *    - Detects fault clearing edges (no hardware interrupt for deassertions)
 *    - Detects secondary faults (FAULT_STAT already HIGH prevents new edge)
 *    - Always rearms on read failure to prevent stall
 *
 * 3. Probe Initialization (mc33978_hwmon_probe):
 *    - Reads initial fault state after IRQ registration
 *    - Clears power-on-reset (POR) flag from hardware
 *    - Primes last_faults cache before first interrupt
 *
 * Paths 1 and 2 are serialized by hwmon_lock() to prevent race conditions
 * with the clear-on-read Fault register. Sysfs attribute reads are
 * non-destructive (return cached values only, no register access).
 *
 * The Fault register is marked as volatile+precious in regmap configuration,
 * which excludes it from regmap's debugfs register dumps, preventing
 * accidental side effects from debug inspection.
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

/* Operating Temperature Ranges (Datasheet Rated) */
#define MC33978_TEMP_MIN_MC		(-40000)
#define MC33978_TEMP_MAX_MC		125000
#define MC34978_TEMP_MAX_MC		105000

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

#define MC33978_FAULT_POLL_INTERVAL_MS	1000

enum mc33978_hwmon_in_channels {
	MC33978_IN_VBATP,
	MC33978_IN_VDDQ,
};

struct mc33978_hwmon_priv {
	struct device *dev;
	struct device *hwmon_dev;
	struct regmap *map;

	const struct mc33978_hwmon_hw_info *hw_info;

	int fault_irq;

	/* Cached hwmon alarm bits, serialized by hwmon_lock(). */
	u32 last_faults;

	/*
	 * Background polling worker. Active only when faults are present
	 * to compensate for the lack of clearing/secondary edge interrupts.
	 */
	struct delayed_work poll_work;
};

struct mc33978_hwmon_hw_info {
	long rated_max_temp;
};

static const struct mc33978_hwmon_hw_info hwmon_hwinfo_mc33978 = {
	.rated_max_temp = MC33978_TEMP_MAX_MC,
};

static const struct mc33978_hwmon_hw_info hwmon_hwinfo_mc34978 = {
	.rated_max_temp = MC34978_TEMP_MAX_MC,
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
	if (!new_faults)
		return;

	if (new_faults & MC33978_FAULT_TEMP_WARN)
		dev_warn_ratelimited(priv->dev, "Temperature warning threshold reached\n");

	if (new_faults & MC33978_FAULT_OT)
		dev_crit_ratelimited(priv->dev, "Over-temperature fault detected!\n");

	if (new_faults & MC33978_FAULT_OV)
		dev_crit_ratelimited(priv->dev, "Over-voltage fault detected!\n");

	if (new_faults & MC33978_FAULT_UV)
		dev_err_ratelimited(priv->dev, "Under-voltage fault detected!\n");
}

static int mc33978_hwmon_update_faults(struct mc33978_hwmon_priv *priv)
{
	u32 old_faults, new_faults, changed_faults;
	u32 alarm_faults = 0;
	u32 faults = 0;
	bool rearm;
	int ret;

	/*
	 * Serialize clear-on-read FAULT register access with cached alarm state
	 * updates and hwmon sysfs readers.
	 */
	hwmon_lock(priv->hwmon_dev);
	old_faults = priv->last_faults;

	ret = mc33978_hwmon_read_fault(priv, &faults);
	if (ret) {
		hwmon_unlock(priv->hwmon_dev);
		dev_err_ratelimited(priv->dev,
				    "failed to read fault register: %pe\n",
				    ERR_PTR(ret));
		/*
		 * Always retry on read failure. If we drop the heartbeat during
		 * the initial fault before caching it, the edge-triggered IRQ
		 * will never fire again and permanently stall fault monitoring.
		 */
		rearm = true;
		goto out_poll;
	}

	/* Isolate hwmon alarm bits from system integrity bits */
	alarm_faults = faults & MC33978_FAULT_ALARM_MASK;
	changed_faults = alarm_faults ^ old_faults;
	new_faults = alarm_faults & ~old_faults;
	priv->last_faults = alarm_faults;

	hwmon_unlock(priv->hwmon_dev);

	if (faults & MC33978_FAULT_SPI_ERROR)
		dev_err_ratelimited(priv->dev, "SPI communication error detected\n");
	if (faults & MC33978_FAULT_HASH)
		dev_err_ratelimited(priv->dev, "SPI register hash mismatch detected\n");

	if (new_faults)
		mc33978_hwmon_report_faults(priv, new_faults);

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

	if (changed_faults)
		hwmon_notify_event(priv->hwmon_dev, hwmon_chip,
				   hwmon_chip_alarms, 0);

	rearm = !!alarm_faults;

out_poll:
	/*
	 * If any alarms are currently active, the global FAULT_STAT bit remains
	 * asserted. The hardware will not generate a new rising edge interrupt
	 * if a secondary fault occurs, nor will it interrupt when faults clear.
	 * Schedule a poll to detect both clearing edges and secondary alarms.
	 */
	if (rearm)
		/* Use freezable polling to pause while the system is suspended. */
		mod_delayed_work(system_freezable_wq, &priv->poll_work,
				 msecs_to_jiffies(MC33978_FAULT_POLL_INTERVAL_MS));

	return ret;
}

static irqreturn_t mc33978_hwmon_fault_irq(int irq, void *data)
{
	struct mc33978_hwmon_priv *priv = data;

	mc33978_hwmon_update_faults(priv);

	return IRQ_HANDLED;
}

static void mc33978_hwmon_poll_work(struct work_struct *work)
{
	struct mc33978_hwmon_priv *priv =
		container_of(work, struct mc33978_hwmon_priv, poll_work.work);

	mc33978_hwmon_update_faults(priv);
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
		case hwmon_temp_rated_min:
		case hwmon_temp_rated_max:
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

static int mc33978_hwmon_read_chip(struct mc33978_hwmon_priv *priv, u32 attr,
				   long *val)
{
	if (attr == hwmon_chip_alarms) {
		*val = priv->last_faults;
		return 0;
	}

	return -EOPNOTSUPP;
}

static int mc33978_hwmon_read_in_vbatp(struct mc33978_hwmon_priv *priv,
				       u32 attr, long *val)
{
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
	case hwmon_in_crit_alarm:
		*val = !!(priv->last_faults & MC33978_FAULT_OV);
		return 0;
	case hwmon_in_lcrit_alarm:
		*val = !!(priv->last_faults & MC33978_FAULT_UV);
		return 0;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int mc33978_hwmon_read_in_vddq(u32 attr, long *val)
{
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

	return -EOPNOTSUPP;
}

static int mc33978_hwmon_read_in(struct mc33978_hwmon_priv *priv, u32 attr,
				 int channel, long *val)
{
	switch (channel) {
	case MC33978_IN_VBATP:
		return mc33978_hwmon_read_in_vbatp(priv, attr, val);
	case MC33978_IN_VDDQ:
		return mc33978_hwmon_read_in_vddq(attr, val);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int mc33978_hwmon_read_temp(struct mc33978_hwmon_priv *priv, u32 attr,
				   long *val)
{
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
	case hwmon_temp_max_alarm:
		*val = !!(priv->last_faults & MC33978_FAULT_TEMP_WARN);
		return 0;
	case hwmon_temp_crit_alarm:
		*val = !!(priv->last_faults & MC33978_FAULT_OT);
		return 0;
	case hwmon_temp_rated_min:
		*val = MC33978_TEMP_MIN_MC;
		return 0;
	case hwmon_temp_rated_max:
		*val = priv->hw_info->rated_max_temp;
		return 0;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int mc33978_hwmon_read(struct device *dev,
			      enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct mc33978_hwmon_priv *priv = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_chip:
		return mc33978_hwmon_read_chip(priv, attr, val);
	case hwmon_in:
		return mc33978_hwmon_read_in(priv, attr, channel, val);
	case hwmon_temp:
		return mc33978_hwmon_read_temp(priv, attr, val);
	default:
		break;
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
			   HWMON_T_MAX_ALARM | HWMON_T_CRIT_ALARM |
			   HWMON_T_RATED_MIN | HWMON_T_RATED_MAX),
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

static void mc33978_hwmon_action_cancel_work(void *data)
{
	struct mc33978_hwmon_priv *priv = data;

	cancel_delayed_work_sync(&priv->poll_work);
}

static int mc33978_hwmon_probe(struct platform_device *pdev)
{
	const struct platform_device_id *id;
	struct device *dev = &pdev->dev;
	struct mc33978_hwmon_priv *priv;
	struct device *hwmon_dev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	id = platform_get_device_id(pdev);
	if (!id || !id->driver_data)
		return dev_err_probe(dev, -EINVAL, "missing device match data\n");

	priv->hw_info = (const struct mc33978_hwmon_hw_info *)id->driver_data;

	priv->map = dev_get_regmap(dev->parent, NULL);
	if (!priv->map)
		return dev_err_probe(dev, -ENODEV, "failed to get regmap\n");

	platform_set_drvdata(pdev, priv);

	INIT_DELAYED_WORK(&priv->poll_work, mc33978_hwmon_poll_work);

	priv->fault_irq = platform_get_irq(pdev, 0);
	if (priv->fault_irq < 0)
		return priv->fault_irq;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "mc33978", priv,
							 &mc33978_hwmon_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return dev_err_probe(dev, PTR_ERR(hwmon_dev),
				     "failed to register hwmon device\n");

	priv->hwmon_dev = hwmon_dev;

	ret = devm_add_action_or_reset(dev, mc33978_hwmon_action_cancel_work,
				       priv);
	if (ret)
		return ret;

	/*
	 * The FAULT child IRQ is generated by the MFD core from transitions of
	 * the aggregated FAULT_STAT bus state. Request a rising-edge nested
	 * IRQ so the core dispatches the hwmon fault handler when faults become
	 * active.
	 *
	 * Fault clearing and secondary faults while FAULT_STAT remains asserted
	 * are handled by the hwmon polling path.
	 */
	ret = devm_request_threaded_irq(dev, priv->fault_irq, NULL,
					mc33978_hwmon_fault_irq,
					IRQF_ONESHOT | IRQF_TRIGGER_RISING,
					dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request fault IRQ\n");

	return mc33978_hwmon_update_faults(priv);
}

static const struct platform_device_id mc33978_hwmon_id[] = {
	{ .name = "mc33978-hwmon", .driver_data = (kernel_ulong_t)&hwmon_hwinfo_mc33978 },
	{ .name = "mc34978-hwmon", .driver_data = (kernel_ulong_t)&hwmon_hwinfo_mc34978 },
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
