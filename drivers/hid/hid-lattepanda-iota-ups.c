// SPDX-License-Identifier: GPL-2.0
#include <linux/power_supply.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/hid.h>
#include <linux/usb.h>
#include "hid-ids.h"

#define REPORT_ID_CAPACITY	0x0C
#define REPORT_ID_STATUS	0x07

#define STATUS_DISCHARGING	BIT(1)
#define STATUS_PLUGGED_IN	BIT(0)
#define STATUS_CHARGING		BIT(2)

MODULE_AUTHOR("Andrew Maney");
MODULE_DESCRIPTION("LattePanda IOTA UPS power supply driver");
MODULE_LICENSE("GPL");

struct iota_ups {
	struct power_supply_desc psu_desc;
	struct power_supply *psu;
	struct hid_device *hiddev;
	spinlock_t lock; /* Protects cached HID report values */

	/* Cached values that are updated from HID reports */
	bool plugged_in;
	char serial[64];
	int charge_limit;
	int psu_status;
	int capacity;

	/*
	 * Wait for both status and capacity reports before registering
	 * with the power_supply core, so initial values are correct and
	 * not erroneous.
	 */
	struct completion got_initial_data;
	struct work_struct register_work;
	bool got_capacity;
	bool data_ready;
	bool got_status;
};

static enum power_supply_property iota_ups_properties[] = {
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_SCOPE,
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

	/* Remaining capacity as a percentage from 0 to 100 */
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = ups->capacity;
		break;

	/* The UPS is always present if the driver is loaded */
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;

	/* Whether mains power is connected */
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = ups->plugged_in ? 1 : 0;
		break;

	/*
	 * The UPS board supplies power to the IOTA and any
	 * peripherals connected to it, therefore its scope
	 * is system-wide.
	 */
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;

	/* V1.0 only accepts 18650 Li-ion cells */
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;

	/* 80% or 100%, configured via a DIP switch on the UPS board */
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		val->intval = ups->charge_limit;
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
		unsigned long flags;

		/*
		 * V1.0 supports 80% and 100% charge limits only, which is
		 * set via a DIP switch on the board. This property allows
		 * userspace to inform the driver which limit is configured.
		 */
		if (val->intval != 80 && val->intval != 100)
			return -EINVAL;

		spin_lock_irqsave(&ups->lock, flags);
		ups->charge_limit = val->intval;
		spin_unlock_irqrestore(&ups->lock, flags);
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

	/* All of the UPS's reports are at least 2 bytes */
	if (size < 2)
		return 0;

	spin_lock_irqsave(&ups->lock, flags);

	switch (data[0]) {
	case REPORT_ID_STATUS: {
		u8 status = data[1];
		int new_status;
		bool plugged_in = !!(status & STATUS_PLUGGED_IN);

		/*
		 * The UPS status is determined as follows:
		 * Battery full:
		 *	UPS is plugged in
		 *	Battery is at full capacity
		 *
		 * Battery charging:
		 *	UPS is plugged in
		 *	Battery is not at full capacity
		 *
		 * Battery discharging:
		 *	UPS is not plugged in
		 *
		 * Battery not charging:
		 *	UPS is plugged in
		 *	UPS has halted charging for some reason
		 *
		 * Unknown:
		 *	None of the above conditions are met
		 */
		if (status & STATUS_CHARGING) {
			if (ups->capacity >= ups->charge_limit)
				new_status = POWER_SUPPLY_STATUS_FULL;
			else
				new_status = POWER_SUPPLY_STATUS_CHARGING;

		} else if (status & STATUS_DISCHARGING) {
			new_status = POWER_SUPPLY_STATUS_DISCHARGING;

		} else if (plugged_in) {
			new_status = POWER_SUPPLY_STATUS_NOT_CHARGING;

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
	 * Signal that the UPS is ready to be registered because we have
	 * received both capacity and status reports.
	 */
	if (!ups->data_ready && ups->got_status && ups->got_capacity) {
		ups->data_ready = true;
		complete(&ups->got_initial_data);
	}

	spin_unlock_irqrestore(&ups->lock, flags);

	/*
	 * Notify the power_supply core outside the spinlock to avoid
	 * a deadlock; power_supply_changed() may call back into
	 * get_property() which acquires the same lock.
	 */
	if (changed && ups->psu)
		power_supply_changed(ups->psu);

	return 0;
}

static void iota_ups_register_work(struct work_struct *work)
{
	struct iota_ups *ups = container_of(work, struct iota_ups, register_work);
	struct power_supply_config psu_config = {};
	struct power_supply *psu;

	/*
	 * Wait for both status and capacity reports before registering.
	 * The device sends reports every ~1 second, so 3 seconds is safe.
	 * We wait here in order to prevent registration in an unknown
	 * state, since this could cause emergency shutdowns or other
	 * undesired effects.
	 */
	wait_for_completion_timeout(&ups->got_initial_data,
				    msecs_to_jiffies(3000));

	/* Configure the UPS's power supply properties */
	ups->psu_desc.name = devm_kasprintf(&ups->hiddev->dev, GFP_KERNEL,
					    "lattepanda-iota-ups.%s",
					    dev_name(&ups->hiddev->dev));

	if (!ups->psu_desc.name) {
		hid_err(ups->hiddev, "failed to allocate power supply name\n");
		return;
	}

	ups->psu_desc.property_is_writeable = iota_ups_property_is_writable;
	ups->psu_desc.num_properties	    = ARRAY_SIZE(iota_ups_properties);
	ups->psu_desc.get_property	    = iota_ups_get_property;
	ups->psu_desc.set_property	    = iota_ups_set_property;
	ups->psu_desc.properties	    = iota_ups_properties;
	ups->psu_desc.type		    = POWER_SUPPLY_TYPE_BATTERY;
	psu_config.drv_data		    = ups;

	/* Register the UPS as a power_supply device */
	psu = devm_power_supply_register(&ups->hiddev->dev, &ups->psu_desc, &psu_config);
	if (IS_ERR(psu)) {
		hid_err(ups->hiddev, "power supply registration failed: %pe\n", psu);
		return;
	}

	/*
	 * Finally, notify the power_supply core so userspace reads the correct
	 * initial state immediately after registration.
	 */
	ups->psu = psu;
	power_supply_changed(ups->psu);
	hid_info(ups->hiddev, "LattePanda IOTA UPS registered as a power_supply device\n");
}

static int iota_ups_probe(struct hid_device *hdev,
			  const struct hid_device_id *id)
{
	struct iota_ups *ups;
	int ret;

	ups = devm_kzalloc(&hdev->dev, sizeof(*ups), GFP_KERNEL);
	if (!ups)
		return -ENOMEM;

	ups->hiddev = hdev;
	ups->psu_status = POWER_SUPPLY_STATUS_UNKNOWN;

	/* 50% is a safe default if wait_for_completion_timeout() times out. */
	ups->capacity = 50;

	/*
	 * Default to 100% to prevent unexpected shutdowns.
	 * Userspace can update this via charge_control_end_threshold.
	 */
	ups->charge_limit = 100;

	init_completion(&ups->got_initial_data);
	spin_lock_init(&ups->lock);
	hid_set_drvdata(hdev, ups);

	/*
	 * Retrieve the UPS's serial number from the USB descriptor. If the device is not
	 * a USB device, we can use the unique device identifier as the serial number.
	 */
	if (hid_is_usb(hdev)) {
		struct usb_device *udev = to_usb_device(hdev->dev.parent->parent);

		if (udev->serial)
			strscpy(ups->serial, udev->serial, sizeof(ups->serial));
		else
			strscpy(ups->serial, "Unknown", sizeof(ups->serial));
	} else {
		if (*hdev->uniq)
			strscpy(ups->serial, hdev->uniq, sizeof(ups->serial));
		else
			strscpy(ups->serial, "Unknown", sizeof(ups->serial));
	}

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "HID parse failed: %pe\n", ERR_PTR(ret));
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "HID hw start failed: %pe\n", ERR_PTR(ret));
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "HID hw open failed: %pe\n", ERR_PTR(ret));
		goto err_stop;
	}

	/* Probe for the UPS in a worker queue so we don't halt the enumeration thread */
	INIT_WORK(&ups->register_work, iota_ups_register_work);
	schedule_work(&ups->register_work);
	return 0;

err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void iota_ups_remove(struct hid_device *hdev)
{
	struct iota_ups *ups = hid_get_drvdata(hdev);

	cancel_work_sync(&ups->register_work);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static struct hid_driver iota_ups_driver = {
	.name = "lattepanda-iota-ups",
	.id_table = iota_ups_devices,
	.probe = iota_ups_probe,
	.remove = iota_ups_remove,
	.raw_event = iota_ups_raw_event,
};
module_hid_driver(iota_ups_driver);
