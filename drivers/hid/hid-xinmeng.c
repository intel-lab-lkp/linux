// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Xinmeng M71 Keyboard.
 *
 *  Copyright (c) 2025 Saalim Quadri <danascape@gmail.com>
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/usb.h>

#include "hid-ids.h"

#define	BATTERY_REPORT_ID	(0x13)

struct xinmeng_drvdata {
	struct hid_device *hdev;
	bool online;

	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	u8 battery_capacity;
	bool battery_charging;
};

static enum power_supply_property xinmeng_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_ONLINE
};

static int xinmeng_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct xinmeng_drvdata *drv_data = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = drv_data->online;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (drv_data->online)
			val->intval = drv_data->battery_charging ?
					POWER_SUPPLY_STATUS_CHARGING :
					POWER_SUPPLY_STATUS_DISCHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = drv_data->battery_capacity;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = drv_data->hdev->name;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int xinmeng_battery_probe(struct hid_device *hdev)
{
	struct xinmeng_drvdata *drv_data = hid_get_drvdata(hdev);
	struct power_supply_config pscfg = { .drv_data = drv_data };
	int ret = 0;

	drv_data->online = false;
	drv_data->battery_capacity = 0;

	drv_data->battery_desc.name = "xinmeng-m71-battery";
	drv_data->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	drv_data->battery_desc.properties = xinmeng_battery_props;
	drv_data->battery_desc.num_properties = ARRAY_SIZE(xinmeng_battery_props);
	drv_data->battery_desc.get_property = xinmeng_battery_get_property;

	drv_data->battery = devm_power_supply_register(&hdev->dev,
						       &drv_data->battery_desc, &pscfg);

	if (IS_ERR(drv_data->battery)) {
		ret = PTR_ERR(drv_data->battery);
		drv_data->battery = NULL;
		hid_err(hdev, "Unable to register battery device\n");
		return ret;
	}

	power_supply_powers(drv_data->battery, &hdev->dev);

	return ret;
}

static int xinmeng_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct xinmeng_drvdata *drv_data;
	struct usb_interface *usbif;

	if (!hid_is_usb(hdev))
		return -EINVAL;

	usbif = to_usb_interface(hdev->dev.parent);

	drv_data = devm_kzalloc(&hdev->dev, sizeof(*drv_data), GFP_KERNEL);
	if (!drv_data)
		return -ENOMEM;

	hid_set_drvdata(hdev, drv_data);
	drv_data->hdev = hdev;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	if (usbif->cur_altsetting->desc.bInterfaceNumber == 1) {
		ret = xinmeng_battery_probe(hdev);
		if (ret < 0) {
			hid_err(hdev, "Xinmeng hid battery_probe failed: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int xinmeng_raw_event(struct hid_device *hdev,
				 struct hid_report *report, u8 *data, int size)
{
	struct xinmeng_drvdata *drv_data = hid_get_drvdata(hdev);

	if (drv_data->battery && data[0] == BATTERY_REPORT_ID) {
		u8 charging_status = data[7];
		bool is_charging = (charging_status == 0x11 || charging_status == 0x10);

		drv_data->battery_capacity = data[6];
		drv_data->battery_charging = is_charging;
		drv_data->online = true;
	}

	return 0;
}

static const struct hid_device_id xinmeng_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_XINMENG, USB_DEVICE_ID_XINMENG_M71_RECEIVER) },
	{ }
};
MODULE_DEVICE_TABLE(hid, xinmeng_devices);

static struct hid_driver xinmeng_driver = {
	.name = "xinmeng",
	.id_table = xinmeng_devices,
	.probe = xinmeng_probe,
	.raw_event = xinmeng_raw_event
};
module_hid_driver(xinmeng_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HID driver for Xinmeng keyboards");
MODULE_AUTHOR("Saalim Quadri <danascape@gmail.com>");
