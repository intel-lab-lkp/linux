// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID battery driver for the UNCTRL RAGE Wireless Mouse (model MSPTM101,
 * known as "UNCTRL M1" over Bluetooth. Mad Catz, receiver 0xA0AF,
 * wired 0xA09F). The mouse also connects over Bluetooth, on which no
 * battery queries are made.
 *
 * The same mouse appears as two USB devices at once when it is both
 * docked on the cable and linked to the receiver. Without care that would
 * expose two batteries for one mouse, and only the cable connection
 * actually reaches it, so the driver keeps a single power supply and
 * hands it to whichever connection can answer queries.
 *
 * The device echoes every query before answering. The mode byte in the
 * reply tells the answer from the echoes. The wired connection only
 * answers over the interrupt out endpoint, where the queries go.
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "hid-ids.h"

#define UNCTRL_VENDOR_IFNUM 2

#define UNCTRL_MODE_RECEIVER 0x01
#define UNCTRL_MODE_WIRED 0x44

/* the battery query: report number, 4-byte header, padding, terminator */
static const u8 unctrl_battery_query[33] = {
	0x00,
	0x0A, 0xAA, 0xFF, 0xDD,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00,
	0x6F,
};

struct unctrl_dev {
	struct hid_device *hdev;

	struct power_supply_desc battery_desc;
	struct power_supply *battery;

	u8 battery_capacity;
	u8 mode;
	bool online;
	bool wired;
};

static enum power_supply_property unctrl_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,     POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,   POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME, POWER_SUPPLY_PROP_SERIAL_NUMBER,
	POWER_SUPPLY_PROP_ONLINE,
};

static int unctrl_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct unctrl_dev *dev = power_supply_get_drvdata(psy);
	int ret = 0;
	u8 capacity, mode;
	bool online;

	capacity = dev->battery_capacity;
	mode = dev->mode;
	online = dev->online;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = online;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!online)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		else if (mode == UNCTRL_MODE_WIRED && capacity < 100)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (capacity >= 100)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = capacity;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = dev->hdev->name;
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = "unctrl-m1";
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void unctrl_fetch_battery(struct hid_device *hdev)
{
	u8 *write_buf;
	int ret;

	/* buf[0] is the report number, and hid_hw_output_report() sends the
	 * buffer verbatim.
	 */
	write_buf = kmemdup(unctrl_battery_query, sizeof(unctrl_battery_query),
			    GFP_KERNEL);
	if (!write_buf)
		return;

	ret = hid_hw_output_report(hdev, write_buf,
				   sizeof(unctrl_battery_query));
	if (ret < 0)
		hid_dbg(hdev, "%s:battery query failed with error %d\n",
			__func__, ret);

	kfree(write_buf);
}

/* Register the power supply of this connection and start reporting. */
static int unctrl_battery_register(struct unctrl_dev *dev)
{
	struct power_supply_config config = { .drv_data = dev };
	struct power_supply *battery;
	int ret;

	battery = power_supply_register(&dev->hdev->dev, &dev->battery_desc,
					&config);
	if (IS_ERR(battery)) {
		ret = PTR_ERR(battery);
		hid_err(dev->hdev,
			"%s:power_supply_register failed with error %d\n",
			__func__, ret);
		return ret;
	}

	power_supply_powers(battery, &dev->hdev->dev);
	dev->battery = battery;
	hid_info(dev->hdev, "%s:battery registered as %s\n", __func__,
		 dev->battery_desc.name);

	unctrl_fetch_battery(dev->hdev);

	return 0;
}

static int unctrl_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct unctrl_dev *dev;
	struct usb_interface *usbif;
	unsigned int connect_mask;
	bool vendor;
	int ret;

	if (!hid_is_usb(hdev))
		return -EINVAL;

	usbif = to_usb_interface(hdev->dev.parent);
	vendor = usbif->cur_altsetting->desc.bInterfaceNumber ==
		 UNCTRL_VENDOR_IFNUM;

	/* the pointer, buttons and hidraw have to keep working */
	connect_mask = vendor ? HID_CONNECT_HIDRAW : HID_CONNECT_DEFAULT;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "%s:parse failed with error %d\n", __func__, ret);
		return ret;
	}

	ret = hid_hw_start(hdev, connect_mask);
	if (ret) {
		hid_err(hdev, "%s:hw start failed with error %d\n", __func__,
			ret);
		return ret;
	}

	if (!vendor)
		return 0;

	dev = kzalloc_obj(*dev);
	if (!dev) {
		hid_hw_stop(hdev);
		return -ENOMEM;
	}

	dev->hdev = hdev;
	dev->battery_capacity = 100;
	dev->mode = UNCTRL_MODE_RECEIVER;
	dev->wired = hdev->product == USB_DEVICE_ID_UNCTRL_M1_WIRED;

	dev->battery_desc.name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
						"unctrl-m1-%04x-battery",
						hdev->product);
	if (!dev->battery_desc.name) {
		hid_hw_stop(hdev);
		kfree(dev);
		return -ENOMEM;
	}
	dev->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	dev->battery_desc.properties = unctrl_battery_props;
	dev->battery_desc.num_properties = ARRAY_SIZE(unctrl_battery_props);
	dev->battery_desc.get_property = unctrl_battery_get_property;
	dev->battery_desc.use_for_apm = 0;

	/* usbhid polls the interrupt in endpoint only while it is open */
	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "%s:open failed with error %d\n", __func__, ret);
		hid_hw_stop(hdev);
		kfree(dev);
		return ret;
	}

	hid_set_drvdata(hdev, dev);

	ret = unctrl_battery_register(dev);
	if (ret)
		goto err_del;

	return 0;

err_del:
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	hid_set_drvdata(hdev, NULL);
	kfree(dev);
	return ret;
}

static int unctrl_raw_event(struct hid_device *hdev, struct hid_report *report,
			    u8 *data, int size)
{
	struct unctrl_dev *dev = hid_get_drvdata(hdev);
	struct power_supply *battery = NULL;

	if (!dev || size < 8)
		return 0;

	if (data[0] != 0x0A || data[1] != 0xAA || data[2] != 0xFF ||
	    data[3] != 0xDD)
		return 0;

	/* real answers carry the mode here, echoes of the query do not */
	if (data[5] != UNCTRL_MODE_RECEIVER && data[5] != UNCTRL_MODE_WIRED)
		return 0;

	/* the receiver reports 0% while the mouse is switched off */
	dev->battery_capacity = min_t(u8, data[4], 100);
	dev->mode = data[5];
	dev->online = dev->battery_capacity != 0;
	battery = dev->battery;

	/* only schedules a work, so this is safe from the completion path */
	if (battery)
		power_supply_changed(battery);

	return 0;
}

static void unctrl_remove(struct hid_device *hdev)
{
	struct unctrl_dev *dev = hid_get_drvdata(hdev);

	hid_hw_close(hdev);

	hid_hw_stop(hdev);

	if (!dev)
		return;

	if (dev->battery)
		power_supply_unregister(dev->battery);

	hid_set_drvdata(hdev, NULL);
	kfree(dev);
}

static const struct hid_device_id unctrl_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_MADCATZ,
			 USB_DEVICE_ID_UNCTRL_M1_DONGLE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MADCATZ,
			 USB_DEVICE_ID_UNCTRL_M1_WIRED) },
	{}
};
MODULE_DEVICE_TABLE(hid, unctrl_devices);

static struct hid_driver unctrl_driver = {
	.name = "unctrl-m1",
	.id_table = unctrl_devices,
	.probe = unctrl_probe,
	.raw_event = unctrl_raw_event,
	.remove = unctrl_remove,
};
module_hid_driver(unctrl_driver);

MODULE_AUTHOR("Bishwa Bhusan Saha <contact@bishwasaha.com>");
MODULE_DESCRIPTION("HID battery driver for the UNCTRL M1 Mouse");
MODULE_LICENSE("GPL");
