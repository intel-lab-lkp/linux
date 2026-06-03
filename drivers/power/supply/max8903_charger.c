// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * max8903_charger.c - Maxim 8903 USB/Adapter Charger Driver
 *
 * Copyright (C) 2011 Samsung Electronics
 * MyungJoo Ham <myungjoo.ham@samsung.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/power_supply.h>
#include <linux/platform_device.h>

struct max8903_current_limit_mapping {
	u32 limit_ua;		/* Current limit in microamps */
	u32 gpio_value;		/* GPIO bit pattern */
};

struct max8903_data {
	struct device *dev;
	struct power_supply *psy;
	struct power_supply_desc psy_desc;
	/*
	 * GPIOs
	 * chg, flt, dcm and usus are optional.
	 * dok or uok must be present.
	 * If dok is present, cen must be present.
	 */
	struct gpio_desc *cen; /* Charger Enable input */
	struct gpio_desc *dok; /* DC (Adapter) Power OK output */
	struct gpio_desc *uok; /* USB Power OK output */
	struct gpio_desc *chg; /* Charger status output */
	struct gpio_desc *flt; /* Fault output */
	struct gpio_desc *dcm; /* Current-Limit Mode input (1: DC, 2: USB) */
	struct gpio_desc *usus; /* USB Suspend Input (1: suspended) */

	/* DC current limit control (ISET pins) */
	struct gpio_descs *dc_current_limit_gpios;
	struct max8903_current_limit_mapping *dc_current_limit_map;
	u32 dc_current_limit_map_size;
	u32 dc_current_limit_ua;	/* Current setting in uA */

	/* USB current limit control (IUSB pin) */
	struct gpio_desc *usb_current_limit_gpio;
	u32 usb_current_limit_low_ua;	/* Current when GPIO low */
	u32 usb_current_limit_high_ua;	/* Current when GPIO high */
	u32 usb_current_limit_ua;	/* Current setting in uA */

	/*
	 * Serialises ta_in / usb_in updates against
	 * max8903_set_property() which steers the current-limit write to
	 * the DC or USB path based on which source is currently online.
	 * The IRQ handlers are requested with IRQF_ONESHOT (threaded), so
	 * a sleepable mutex is the right primitive in both contexts.
	 */
	struct mutex source_lock;
	bool fault;
	bool usb_in;
	bool ta_in;
};

static enum power_supply_property max8903_charger_props[] = {
	POWER_SUPPLY_PROP_STATUS, /* Charger status output */
	POWER_SUPPLY_PROP_ONLINE, /* External power source */
	POWER_SUPPLY_PROP_HEALTH, /* Fault or OK */
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, /* Input current limit */
};

static int max8903_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct max8903_data *data = power_supply_get_drvdata(psy);
	bool ta_in, usb_in;
	u32 dc_limit, usb_limit;

	/*
	 * Snapshot the source flags and current-limit settings under the
	 * source_lock that the IRQs (max8903_dcin / max8903_usbin) and
	 * max8903_set_property() take when updating them, so we never
	 * observe a torn pair of (source-online flag, current-limit ua).
	 * The gpiod_get_value() reads further down deliberately stay
	 * outside the lock — they hit the GPIO controller, not driver
	 * state, and the IRQs do not touch them under the lock either.
	 */
	mutex_lock(&data->source_lock);
	ta_in = data->ta_in;
	usb_in = data->usb_in;
	dc_limit = data->dc_current_limit_ua;
	usb_limit = data->usb_current_limit_ua;
	mutex_unlock(&data->source_lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		if (data->chg) {
			if (gpiod_get_value(data->chg))
				/* CHG asserted */
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			else if (usb_in || ta_in)
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			else
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		}
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = (ta_in || usb_in) ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		/*
		 * data->fault is a single bool toggled from one IRQ
		 * handler, so a torn read is not possible; no need to
		 * extend source_lock coverage here.
		 */
		val->intval = data->fault ? POWER_SUPPLY_HEALTH_UNSPEC_FAILURE
					  : POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		if (ta_in && data->dc_current_limit_gpios)
			val->intval = dc_limit;
		else if (usb_in && data->usb_current_limit_gpio)
			val->intval = usb_limit;
		else
			return -ENODATA;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int max8903_set_dc_current_limit(struct max8903_data *data, u32 limit_ua)
{
	int i, best_idx = -1;
	u32 best_gpio_value;
	/*
	 * gpio_value is a u32 in the DT mapping and is parse-time
	 * validated to fit in BIT(ndescs); size the bitmap to the full
	 * width of the source u32 so a DT with up to 32 dc-current-limit
	 * GPIOs cannot overflow this stack buffer.
	 */
	DECLARE_BITMAP(values, BITS_PER_TYPE(u32));

	if (!data->dc_current_limit_gpios)
		return -EOPNOTSUPP;

	/*
	 * Find the highest supported current <= requested. Use a -1
	 * "not found" sentinel rather than tracking best_limit > 0 so
	 * that a 0 uA entry (used to disable charging) can be selected
	 * by a 0 uA request.
	 */
	for (i = 0; i < data->dc_current_limit_map_size; i++) {
		if (data->dc_current_limit_map[i].limit_ua > limit_ua)
			continue;
		if (best_idx < 0 ||
		    data->dc_current_limit_map[i].limit_ua >
				data->dc_current_limit_map[best_idx].limit_ua)
			best_idx = i;
	}

	if (best_idx < 0)
		return -EINVAL;

	best_gpio_value = data->dc_current_limit_map[best_idx].gpio_value;
	bitmap_from_arr32(values, &best_gpio_value, BITS_PER_TYPE(u32));
	gpiod_set_array_value_cansleep(data->dc_current_limit_gpios->ndescs,
				       data->dc_current_limit_gpios->desc,
				       data->dc_current_limit_gpios->info,
				       values);

	data->dc_current_limit_ua = data->dc_current_limit_map[best_idx].limit_ua;
	dev_dbg(data->dev, "DC current limit set to %u uA\n",
		data->dc_current_limit_ua);

	return 0;
}

static int max8903_set_usb_current_limit(struct max8903_data *data, u32 limit_ua)
{
	u32 selected;
	int gpio_val;

	if (!data->usb_current_limit_gpio)
		return -EOPNOTSUPP;

	/*
	 * Pick the highest of the two configured limits that does not
	 * exceed the requested cap. Mirror the DC path's policy: if
	 * neither value fits (the request is below even the low limit),
	 * refuse the request rather than silently program a higher
	 * current that violates the system power budget.
	 */
	if (limit_ua >= data->usb_current_limit_high_ua) {
		selected = data->usb_current_limit_high_ua;
		gpio_val = 1;
	} else if (limit_ua >= data->usb_current_limit_low_ua) {
		selected = data->usb_current_limit_low_ua;
		gpio_val = 0;
	} else {
		return -EINVAL;
	}

	gpiod_set_value_cansleep(data->usb_current_limit_gpio, gpio_val);
	data->usb_current_limit_ua = selected;

	dev_dbg(data->dev, "USB current limit set to %u uA\n",
		data->usb_current_limit_ua);

	return 0;
}

static int max8903_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct max8903_data *data = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		/*
		 * Hold source_lock across the source check and the
		 * resulting hardware write so the IRQ handler cannot
		 * flip ta_in/usb_in between them and have us program the
		 * limit for a source that has just gone offline.
		 */
		mutex_lock(&data->source_lock);
		if (data->ta_in && data->dc_current_limit_gpios)
			ret = max8903_set_dc_current_limit(data, val->intval);
		else if (data->usb_in && data->usb_current_limit_gpio)
			ret = max8903_set_usb_current_limit(data, val->intval);
		else
			ret = -EINVAL;
		mutex_unlock(&data->source_lock);
		return ret;
	default:
		return -EINVAL;
	}
}

static int max8903_property_is_writeable(struct power_supply *psy,
		enum power_supply_property psp)
{
	struct max8903_data *data = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return data->dc_current_limit_gpios ||
		       data->usb_current_limit_gpio;
	default:
		return 0;
	}
}

static irqreturn_t max8903_dcin(int irq, void *_data)
{
	struct max8903_data *data = _data;
	bool ta_in;
	enum power_supply_type old_type;

	/*
	 * This means the line is asserted.
	 *
	 * The signal is active low, but the inversion is handled in the GPIO
	 * library as the line should be flagged GPIO_ACTIVE_LOW in the device
	 * tree.
	 */
	/*
	 * Sample the line under source_lock so a concurrent
	 * max8903_set_property() observes either the old or the new
	 * state consistently, never a torn read where the lock is held
	 * but the cached flag is about to be updated.
	 */
	mutex_lock(&data->source_lock);
	ta_in = gpiod_get_value(data->dok);
	if (ta_in == data->ta_in) {
		mutex_unlock(&data->source_lock);
		return IRQ_HANDLED;
	}

	data->ta_in = ta_in;
	mutex_unlock(&data->source_lock);

	/* Set Current-Limit-Mode 1:DC 0:USB */
	if (data->dcm)
		gpiod_set_value(data->dcm, ta_in);

	/* Charger Enable / Disable */
	if (data->cen) {
		int val;

		if (ta_in)
			/* Certainly enable if DOK is asserted */
			val = 1;
		else if (data->usb_in)
			/* Enable if the USB charger is enabled */
			val = 1;
		else
			/* Else default-disable */
			val = 0;

		gpiod_set_value(data->cen, val);
	}

	dev_dbg(data->dev, "TA(DC-IN) Charger %s.\n", ta_in ?
			"Connected" : "Disconnected");

	old_type = data->psy_desc.type;

	if (data->ta_in)
		data->psy_desc.type = POWER_SUPPLY_TYPE_MAINS;
	else if (data->usb_in)
		data->psy_desc.type = POWER_SUPPLY_TYPE_USB;
	else
		data->psy_desc.type = POWER_SUPPLY_TYPE_BATTERY;

	if (old_type != data->psy_desc.type)
		power_supply_changed(data->psy);

	return IRQ_HANDLED;
}

static irqreturn_t max8903_usbin(int irq, void *_data)
{
	struct max8903_data *data = _data;
	bool usb_in;
	enum power_supply_type old_type;

	/*
	 * This means the line is asserted.
	 *
	 * The signal is active low, but the inversion is handled in the GPIO
	 * library as the line should be flagged GPIO_ACTIVE_LOW in the device
	 * tree.
	 */
	/* See ta_in handler: sample the line under the lock. */
	mutex_lock(&data->source_lock);
	usb_in = gpiod_get_value(data->uok);
	if (usb_in == data->usb_in) {
		mutex_unlock(&data->source_lock);
		return IRQ_HANDLED;
	}

	data->usb_in = usb_in;
	mutex_unlock(&data->source_lock);

	/* Do not touch Current-Limit-Mode */

	/* Charger Enable / Disable */
	if (data->cen) {
		int val;

		if (usb_in)
			/* Certainly enable if UOK is asserted */
			val = 1;
		else if (data->ta_in)
			/* Enable if the DC charger is enabled */
			val = 1;
		else
			/* Else default-disable */
			val = 0;

		gpiod_set_value(data->cen, val);
	}

	dev_dbg(data->dev, "USB Charger %s.\n", usb_in ?
			"Connected" : "Disconnected");

	old_type = data->psy_desc.type;

	if (data->ta_in)
		data->psy_desc.type = POWER_SUPPLY_TYPE_MAINS;
	else if (data->usb_in)
		data->psy_desc.type = POWER_SUPPLY_TYPE_USB;
	else
		data->psy_desc.type = POWER_SUPPLY_TYPE_BATTERY;

	if (old_type != data->psy_desc.type)
		power_supply_changed(data->psy);

	return IRQ_HANDLED;
}

static irqreturn_t max8903_fault(int irq, void *_data)
{
	struct max8903_data *data = _data;
	bool fault;

	/*
	 * This means the line is asserted.
	 *
	 * The signal is active low, but the inversion is handled in the GPIO
	 * library as the line should be flagged GPIO_ACTIVE_LOW in the device
	 * tree.
	 */
	fault = gpiod_get_value(data->flt);

	if (fault == data->fault)
		return IRQ_HANDLED;

	data->fault = fault;

	if (fault)
		dev_err(data->dev, "Charger suffers a fault and stops.\n");
	else
		dev_err(data->dev, "Charger recovered from a fault.\n");

	return IRQ_HANDLED;
}

static int max8903_parse_dc_current_limit(struct platform_device *pdev,
					  struct max8903_data *data)
{
	struct device *dev = &pdev->dev;
	int ret, i, map_size;
	u32 *map;

	data->dc_current_limit_gpios = devm_gpiod_get_array_optional(dev,
					"dc-current-limit", GPIOD_OUT_LOW);
	if (IS_ERR(data->dc_current_limit_gpios))
		return dev_err_probe(dev, PTR_ERR(data->dc_current_limit_gpios),
				     "failed to get DC current limit GPIOs");

	if (!data->dc_current_limit_gpios)
		return 0;	/* Optional feature not present */

	/* Parse mapping: pairs of (current_ua, gpio_value) */
	map_size = device_property_count_u32(dev, "dc-current-limit-mapping");
	if (map_size <= 0 || map_size % 2) {
		dev_err(dev, "invalid dc-current-limit-mapping\n");
		return -EINVAL;
	}

	map = devm_kcalloc(dev, map_size, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	ret = device_property_read_u32_array(dev, "dc-current-limit-mapping",
					     map, map_size);
	if (ret) {
		dev_err(dev, "failed to read dc-current-limit-mapping\n");
		return ret;
	}

	data->dc_current_limit_map_size = map_size / 2;
	data->dc_current_limit_map = devm_kcalloc(dev,
					data->dc_current_limit_map_size,
					sizeof(*data->dc_current_limit_map),
					GFP_KERNEL);
	if (!data->dc_current_limit_map)
		return -ENOMEM;

	for (i = 0; i < data->dc_current_limit_map_size; i++) {
		u32 gpio_value = map[i * 2 + 1];

		/*
		 * gpio_value is the bitmap programmed across the
		 * dc-current-limit GPIOs, so it cannot represent more
		 * bits than the GPIO array width. A larger value would
		 * be silently truncated by gpiod_set_array_value() and
		 * select the wrong limit; reject it at parse time so
		 * the bogus DT is visible to the integrator.
		 */
		if (gpio_value >= BIT(data->dc_current_limit_gpios->ndescs)) {
			dev_err(dev,
				"dc-current-limit-mapping entry %d: gpio_value 0x%x exceeds %u-GPIO range\n",
				i, gpio_value,
				data->dc_current_limit_gpios->ndescs);
			return -EINVAL;
		}
		data->dc_current_limit_map[i].limit_ua = map[i * 2];
		data->dc_current_limit_map[i].gpio_value = gpio_value;
	}

	/*
	 * devm_gpiod_get_array_optional() above asked for GPIOD_OUT_LOW,
	 * so the hardware mux starts at gpio_value 0. Require the DT
	 * mapping to include a gpio_value=0 entry so the software
	 * current-limit state has a definite initial value matching the
	 * hardware. Without this entry we would have to guess and the
	 * reported INPUT_CURRENT_LIMIT could disagree with what the
	 * mux is actually wired to until a set_property write picks a
	 * real value.
	 */
	for (i = 0; i < data->dc_current_limit_map_size; i++)
		if (data->dc_current_limit_map[i].gpio_value == 0)
			break;
	if (i == data->dc_current_limit_map_size) {
		dev_err(dev,
			"dc-current-limit-mapping must include a gpio_value=0 entry to describe the boot-time mux state\n");
		return -EINVAL;
	}
	data->dc_current_limit_ua = data->dc_current_limit_map[i].limit_ua;

	dev_dbg(dev, "DC current limit control: %d levels available, initial %u uA\n",
		data->dc_current_limit_map_size, data->dc_current_limit_ua);

	return 0;
}

static int max8903_parse_usb_current_limit(struct platform_device *pdev,
					   struct max8903_data *data)
{
	struct device *dev = &pdev->dev;
	u32 limits[2];
	int ret;

	data->usb_current_limit_gpio = devm_gpiod_get_optional(dev,
					"usb-current-limit", GPIOD_OUT_LOW);
	if (IS_ERR(data->usb_current_limit_gpio))
		return dev_err_probe(dev, PTR_ERR(data->usb_current_limit_gpio),
				     "failed to get USB current limit GPIO");

	if (!data->usb_current_limit_gpio)
		return 0;	/* Optional feature not present */

	/* Parse [low_ua, high_ua] values, default to USB spec values */
	ret = device_property_read_u32_array(dev, "usb-current-limit-values",
					     limits, 2);
	if (ret) {
		/* Default to USB spec values */
		data->usb_current_limit_low_ua = 100000;   /* 100mA */
		data->usb_current_limit_high_ua = 500000;  /* 500mA */
	} else {
		data->usb_current_limit_low_ua = limits[0];
		data->usb_current_limit_high_ua = limits[1];
	}

	/*
	 * max8903_set_usb_current_limit() picks the highest cap that
	 * doesn't exceed the request by checking >=high first then
	 * >=low; that policy only works when high > low. Reject DTs
	 * that hand the property in the wrong order rather than
	 * silently program a sub-optimal current limit.
	 */
	if (data->usb_current_limit_high_ua <= data->usb_current_limit_low_ua) {
		dev_err(dev,
			"usb-current-limit-values must be [low, high] with high > low (got low=%u uA, high=%u uA)\n",
			data->usb_current_limit_low_ua,
			data->usb_current_limit_high_ua);
		return -EINVAL;
	}

	/* Start at low current for safety */
	data->usb_current_limit_ua = data->usb_current_limit_low_ua;

	dev_dbg(dev, "USB current limit control: %u/%u uA\n",
		data->usb_current_limit_low_ua, data->usb_current_limit_high_ua);

	return 0;
}

static int max8903_setup_gpios(struct platform_device *pdev)
{
	struct max8903_data *data = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	bool ta_in = false;
	bool usb_in = false;
	enum gpiod_flags flags;

	data->dok = devm_gpiod_get_optional(dev, "dok", GPIOD_IN);
	if (IS_ERR(data->dok))
		return dev_err_probe(dev, PTR_ERR(data->dok),
				     "failed to get DOK GPIO");
	if (data->dok) {
		gpiod_set_consumer_name(data->dok, data->psy_desc.name);
		/*
		 * The DC OK is pulled up to 1 and goes low when a charger
		 * is plugged in (active low) but in the device tree the
		 * line is marked as GPIO_ACTIVE_LOW so we get a 1 (asserted)
		 * here if the DC charger is plugged in.
		 */
		ta_in = gpiod_get_value(data->dok);
	}

	data->uok = devm_gpiod_get_optional(dev, "uok", GPIOD_IN);
	if (IS_ERR(data->uok))
		return dev_err_probe(dev, PTR_ERR(data->uok),
				     "failed to get UOK GPIO");
	if (data->uok) {
		gpiod_set_consumer_name(data->uok, data->psy_desc.name);
		/*
		 * The USB OK is pulled up to 1 and goes low when a USB charger
		 * is plugged in (active low) but in the device tree the
		 * line is marked as GPIO_ACTIVE_LOW so we get a 1 (asserted)
		 * here if the USB charger is plugged in.
		 */
		usb_in = gpiod_get_value(data->uok);
	}

	/* Either DC OK or USB OK must be provided */
	if (!data->dok && !data->uok) {
		dev_err(dev, "no valid power source\n");
		return -EINVAL;
	}

	/*
	 * If either charger is already connected at this point,
	 * assert the CEN line and enable charging from the start.
	 *
	 * The line is active low but also marked with GPIO_ACTIVE_LOW
	 * in the device tree, so when we assert the line with
	 * GPIOD_OUT_HIGH the line will be driven low.
	 */
	flags = (ta_in || usb_in) ? GPIOD_OUT_HIGH : GPIOD_OUT_LOW;
	/*
	 * If DC OK is provided, Charger Enable CEN is compulsory
	 * so this is not optional here.
	 */
	data->cen = devm_gpiod_get(dev, "cen", flags);
	if (IS_ERR(data->cen))
		return dev_err_probe(dev, PTR_ERR(data->cen),
				     "failed to get CEN GPIO");
	gpiod_set_consumer_name(data->cen, data->psy_desc.name);

	/*
	 * If the DC charger is connected, then select it.
	 *
	 * The DCM line should be marked GPIO_ACTIVE_HIGH in the
	 * device tree. Driving it high will enable the DC charger
	 * input over the USB charger input.
	 */
	flags = ta_in ? GPIOD_OUT_HIGH : GPIOD_OUT_LOW;
	data->dcm = devm_gpiod_get_optional(dev, "dcm", flags);
	if (IS_ERR(data->dcm))
		return dev_err_probe(dev, PTR_ERR(data->dcm),
				     "failed to get DCM GPIO");
	gpiod_set_consumer_name(data->dcm, data->psy_desc.name);

	data->chg = devm_gpiod_get_optional(dev, "chg", GPIOD_IN);
	if (IS_ERR(data->chg))
		return dev_err_probe(dev, PTR_ERR(data->chg),
				     "failed to get CHG GPIO");
	gpiod_set_consumer_name(data->chg, data->psy_desc.name);

	data->flt = devm_gpiod_get_optional(dev, "flt", GPIOD_IN);
	if (IS_ERR(data->flt))
		return dev_err_probe(dev, PTR_ERR(data->flt),
				     "failed to get FLT GPIO");
	gpiod_set_consumer_name(data->flt, data->psy_desc.name);

	data->usus = devm_gpiod_get_optional(dev, "usus", GPIOD_IN);
	if (IS_ERR(data->usus))
		return dev_err_probe(dev, PTR_ERR(data->usus),
				     "failed to get USUS GPIO");
	gpiod_set_consumer_name(data->usus, data->psy_desc.name);

	data->fault = false;
	data->ta_in = ta_in;
	data->usb_in = usb_in;

	return 0;
}

static int max8903_probe(struct platform_device *pdev)
{
	struct max8903_data *data;
	struct device *dev = &pdev->dev;
	struct power_supply_config psy_cfg = {};
	int ret = 0;

	data = devm_kzalloc(dev, sizeof(struct max8903_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	mutex_init(&data->source_lock);
	platform_set_drvdata(pdev, data);

	ret = max8903_setup_gpios(pdev);
	if (ret)
		return ret;

	ret = max8903_parse_dc_current_limit(pdev, data);
	if (ret)
		return ret;

	ret = max8903_parse_usb_current_limit(pdev, data);
	if (ret)
		return ret;

	data->psy_desc.name = "max8903_charger";
	data->psy_desc.type = (data->ta_in) ? POWER_SUPPLY_TYPE_MAINS :
			((data->usb_in) ? POWER_SUPPLY_TYPE_USB :
			 POWER_SUPPLY_TYPE_BATTERY);
	data->psy_desc.get_property = max8903_get_property;
	data->psy_desc.set_property = max8903_set_property;
	data->psy_desc.property_is_writeable = max8903_property_is_writeable;
	data->psy_desc.properties = max8903_charger_props;
	data->psy_desc.num_properties = ARRAY_SIZE(max8903_charger_props);

	psy_cfg.fwnode = dev_fwnode(dev);
	psy_cfg.drv_data = data;

	data->psy = devm_power_supply_register(dev, &data->psy_desc, &psy_cfg);
	if (IS_ERR(data->psy)) {
		dev_err(dev, "failed: power supply register.\n");
		return PTR_ERR(data->psy);
	}

	if (data->dok) {
		ret = devm_request_threaded_irq(dev, gpiod_to_irq(data->dok),
					NULL, max8903_dcin,
					IRQF_TRIGGER_FALLING |
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"MAX8903 DC IN", data);
		if (ret) {
			dev_err(dev, "Cannot request irq %d for DC (%d)\n",
					gpiod_to_irq(data->dok), ret);
			return ret;
		}
	}

	if (data->uok) {
		ret = devm_request_threaded_irq(dev, gpiod_to_irq(data->uok),
					NULL, max8903_usbin,
					IRQF_TRIGGER_FALLING |
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"MAX8903 USB IN", data);
		if (ret) {
			dev_err(dev, "Cannot request irq %d for USB (%d)\n",
					gpiod_to_irq(data->uok), ret);
			return ret;
		}
	}

	if (data->flt) {
		ret = devm_request_threaded_irq(dev, gpiod_to_irq(data->flt),
					NULL, max8903_fault,
					IRQF_TRIGGER_FALLING |
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"MAX8903 Fault", data);
		if (ret) {
			dev_err(dev, "Cannot request irq %d for Fault (%d)\n",
					gpiod_to_irq(data->flt), ret);
			return ret;
		}
	}

	return 0;
}

static const struct of_device_id max8903_match_ids[] = {
	{ .compatible = "maxim,max8903", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, max8903_match_ids);

static struct platform_driver max8903_driver = {
	.probe	= max8903_probe,
	.driver = {
		.name	= "max8903-charger",
		.of_match_table = max8903_match_ids
	},
};

module_platform_driver(max8903_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MAX8903 Charger Driver");
MODULE_AUTHOR("MyungJoo Ham <myungjoo.ham@samsung.com>");
MODULE_ALIAS("platform:max8903-charger");
