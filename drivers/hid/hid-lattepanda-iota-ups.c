// SPDX-License-Identifier: GPL-2.0
#include <linux/completion.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include "hid-ids.h"

#define REPORT_ID_STATUS	0x07
#define REPORT_ID_CAPACITY	0x0C

#define STATUS_PLUGGED_IN	BIT(0)
#define STATUS_DISCHARGING	BIT(1)
#define STATUS_CHARGING		BIT(2)

MODULE_AUTHOR("Andrew Maney");
MODULE_DESCRIPTION("LattePanda IOTA UPS power supply driver");
MODULE_LICENSE("GPL");

struct iota_ups {
	struct hid_device		*hiddev;
	struct power_supply		*psu;
	struct power_supply_desc	 psu_desc;
	spinlock_t			 lock; /* Protects all cached HID report values */

	/* Cached values updated from HID reports */
	char	serial[64];
	bool	plugged_in;
	int	psu_status;
	int	capacity;
	int	charge_limit;

	/*
	 * Wait for both status and capacity reports before registering
	 * with the power_supply core, so initial values are correct.
	 */
	struct completion	got_initial_data;
	bool			data_ready;
	bool			got_status;
	bool			got_capacity;
};

static enum power_supply_property iota_ups_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static const struct hid_device_id iota_ups_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LATTEPANDA_IOTA,
			USB_DEVICE_ID_LATTEPANDA_IOTA_UPS) },
	{ }
};
MODULE_DEVICE_TABLE(hid, iota_ups_devices);

static int iota_ups_get_property(struct power_supply *supply,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct iota_ups *ups = power_supply_get_drvdata(supply);
	unsigned long flags;

	spin_lock_irqsave(&ups->lock, flags);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = ups->psu_status;
		break;
	/* Remaining capacity as a percentage, 0-100 */
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = ups->capacity;
		break;
	/* Always present if the driver is loaded */
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	/* Whether mains power is connected */
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = ups->plugged_in ? 1 : 0;
		break;
	/* System-level UPS, not a laptop battery */
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	/* V1.0 only accepts 18650 Li-ion cells */
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	/* 80% or 100%, configured via DIP switch on the board */
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		val->intval = ups->charge_limit;
		break;
	/* V1.0 does not report capacity level via HID */
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		break;
	/* V1.0 does not report time remaining */
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		val->intval = 0;
		break;
	/* V1.0 does not report health; assume good */
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	/* 3.7V in microvolts, typical Li-ion resting voltage */
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = 3700000;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "DFRobot";
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "LattePanda IOTA UPS";
		break;
	/* Retrieved from the USB descriptor */
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = ups->serial;
		break;
	default:
		spin_unlock_irqrestore(&ups->lock, flags);
		return -EINVAL;
	}

	spin_unlock_irqrestore(&ups->lock, flags);
	return 0;
}

static int iota_ups_set_property(struct power_supply *supply,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	struct iota_ups *ups = power_supply_get_drvdata(supply);

	if (psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD) {
		/*
		 * V1.0 supports 80% and 100% charge limits only, set via
		 * DIP switch on the board. This property allows userspace
		 * to inform the driver which limit is configured.
		 */
		if (val->intval != 80 && val->intval != 100)
			return -EINVAL;
		ups->charge_limit = val->intval;
		return 0;
	}

	return -EINVAL;
}

static int iota_ups_property_is_writable(struct power_supply *supply,
					 enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD;
}

static int iota_ups_raw_event(struct hid_device *hdev,
			      struct hid_report *report,
			      u8 *data, int size)
{
	struct iota_ups *ups = hid_get_drvdata(hdev);
	unsigned long flags;
	bool changed = false;

	/* All UPS reports are at least 2 bytes */
	if (size < 2)
		return 0;

	spin_lock_irqsave(&ups->lock, flags);

	switch (data[0]) {
	case REPORT_ID_STATUS: {
		u8 status = data[1];
		int new_status;
		bool plugged_in = !!(status & STATUS_PLUGGED_IN);

		if (status & STATUS_CHARGING) {
			if (ups->capacity >= ups->charge_limit)
				new_status = POWER_SUPPLY_STATUS_FULL;
			else
				new_status = POWER_SUPPLY_STATUS_CHARGING;
		} else if (status & STATUS_DISCHARGING) {
			new_status = POWER_SUPPLY_STATUS_DISCHARGING;
		} else if (plugged_in) {
			new_status = POWER_SUPPLY_STATUS_FULL;
		} else {
			new_status = POWER_SUPPLY_STATUS_UNKNOWN;
		}

		if (new_status != ups->psu_status ||
		    plugged_in != ups->plugged_in) {
			ups->plugged_in = plugged_in;
			ups->psu_status = new_status;
			changed = true;
		}

		ups->got_status = true;
		break;
	}
	case REPORT_ID_CAPACITY: {
		int new_cap = clamp((int)data[1], 0, 100);

		if (new_cap != ups->capacity) {
			ups->capacity = new_cap;
			changed = true;
		}

		ups->got_capacity = true;
		break;
	}
	}

	/*
	 * Signal ready only once both status and capacity have been
	 * received, so the power_supply is registered with valid data.
	 */
	if (!ups->data_ready && ups->got_status && ups->got_capacity) {
		ups->data_ready = true;
		complete(&ups->got_initial_data);
	}

	spin_unlock_irqrestore(&ups->lock, flags);

	/*
	 * Notify the power_supply core outside the spinlock. This
	 * triggers UPower's PropertiesChanged signal with the new values.
	 */
	if (changed && ups->psu)
		power_supply_changed(ups->psu);

	return 0;
}

static int iota_ups_probe(struct hid_device *hdev,
			  const struct hid_device_id *id)
{
	struct power_supply_config psu_config = {};
	struct usb_device *udev;
	struct iota_ups *ups;
	int ret;

	ups = devm_kzalloc(&hdev->dev, sizeof(*ups), GFP_KERNEL);
	if (!ups)
		return -ENOMEM;

	ups->hiddev		= hdev;
	ups->psu_status		= POWER_SUPPLY_STATUS_UNKNOWN;
	ups->capacity		= 50;
	/*
	 * Default to 100% — the DIP switch may be set to 80% but there
	 * is no way to detect this automatically from HID reports.
	 * Userspace can update this via charge_control_end_threshold.
	 */
	ups->charge_limit	= 100;
	ups->data_ready		= false;
	ups->got_status		= false;
	ups->got_capacity	= false;

	init_completion(&ups->got_initial_data);
	spin_lock_init(&ups->lock);
	hid_set_drvdata(hdev, ups);

	/* Retrieve serial number from the USB descriptor */
	udev = to_usb_device(hdev->dev.parent->parent);
	if (udev->serial)
		strscpy(ups->serial, udev->serial, sizeof(ups->serial));
	else
		strscpy(ups->serial, "Unknown", sizeof(ups->serial));

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "HID parse failed: %d\n", ret);
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "HID hw start failed: %d\n", ret);
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "HID hw open failed: %d\n", ret);
		goto err_stop;
	}

	/*
	 * Wait for both status and capacity reports before registering.
	 * The device sends reports every ~1 second; 3 seconds is safe.
	 */
	wait_for_completion_timeout(&ups->got_initial_data,
				    msecs_to_jiffies(3000));

	ups->psu_desc.name			= "lattepanda-iota-ups";
	ups->psu_desc.type			= POWER_SUPPLY_TYPE_BATTERY;
	ups->psu_desc.properties		= iota_ups_properties;
	ups->psu_desc.num_properties		= ARRAY_SIZE(iota_ups_properties);
	ups->psu_desc.get_property		= iota_ups_get_property;
	ups->psu_desc.set_property		= iota_ups_set_property;
	ups->psu_desc.property_is_writeable	= iota_ups_property_is_writable;
	psu_config.drv_data = ups;

	ups->psu = devm_power_supply_register(&hdev->dev,
					      &ups->psu_desc,
					      &psu_config);
	if (IS_ERR(ups->psu)) {
		ret = PTR_ERR(ups->psu);
		hid_err(hdev, "power_supply register failed: %d\n", ret);
		goto err_close;
	}

	/*
	 * Force an immediate notification so UPower reads the correct
	 * initial state right after registration.
	 */
	power_supply_changed(ups->psu);

	hid_info(hdev, "LattePanda IOTA UPS registered as power supply\n");
	return 0;

err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void iota_ups_remove(struct hid_device *hdev)
{
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static struct hid_driver iota_ups_driver = {
	.name		= "lattepanda-iota-ups",
	.id_table	= iota_ups_devices,
	.probe		= iota_ups_probe,
	.remove		= iota_ups_remove,
	.raw_event	= iota_ups_raw_event,
};
module_hid_driver(iota_ups_driver);
