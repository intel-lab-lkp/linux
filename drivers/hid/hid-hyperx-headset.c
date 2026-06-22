// SPDX-License-Identifier: GPL-2.0-only
/*
 *  HID driver for HyperX headsets
 *
 *  Supports HyperX Cloud III Wireless headsets.
 *
 *  Copyright (c) 2026 Sofia Schneider
 */

#include <linux/usb.h>
#include <linux/hid.h>

#include "hid-ids.h"

#define HYPERX_POLL_INTERVAL_MS (2 * 60 * 1000)

#define HYPERX_REPORT_ID 0x66
#define HYPERX_PACKET_SIZE 62

#define HYPERX_CMD_GET_CONNECTED 0x82
#define HYPERX_CMD_GET_BATTERY 0x89
#define HYPERX_CMD_GET_CHARGING 0x8A

#define HYPERX_RESP_CONNECTED 0x0B
#define HYPERX_RESP_CHARGING 0x0C
#define HYPERX_RESP_BATTERY 0x0D

#define HYPERX_PREFIX "HP, Inc "
#define HYPERX_PREFIX_LEN strlen(HYPERX_PREFIX)

struct hyperx_headset_device {
	struct hid_device *hdev;
	struct power_supply *battery;

	spinlock_t lock;
	u8 battery_level;
	bool is_charging;
	bool is_connected;

	struct delayed_work poll_work;
	struct work_struct battery_work;
};

static const enum power_supply_property hyperx_headset_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int hyperx_headset_battery_get_property(struct power_supply *psy,
					       enum power_supply_property psp,
					       union power_supply_propval *val)
{
	struct hyperx_headset_device *drvdata = power_supply_get_drvdata(psy);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&drvdata->lock, flags);

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = drvdata->is_connected ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = drvdata->battery_level;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!drvdata->is_connected)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		else if (drvdata->is_charging)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (drvdata->battery_level == 100)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = drvdata->hdev->name;
		while (!strncmp(val->strval, HYPERX_PREFIX, HYPERX_PREFIX_LEN))
			val->strval += HYPERX_PREFIX_LEN;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "HyperX";
		break;

	default:
		ret = -EINVAL;
		break;
	}

	spin_unlock_irqrestore(&drvdata->lock, flags);
	return ret;
}

static const struct power_supply_desc hyperx_headset_battery_desc = {
	.name = "hyperx_headset_battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = hyperx_headset_battery_props,
	.num_properties = ARRAY_SIZE(hyperx_headset_battery_props),
	.get_property = hyperx_headset_battery_get_property,
};

static int hyperx_headset_send_command(struct hyperx_headset_device *drvdata,
				       u8 command)
{
	struct hid_device *hdev = drvdata->hdev;
	u8 *buf;
	int ret;

	buf = kzalloc(HYPERX_PACKET_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = HYPERX_REPORT_ID;
	buf[1] = command;

	ret = hid_hw_raw_request(hdev, HYPERX_REPORT_ID, buf,
				 HYPERX_PACKET_SIZE, HID_OUTPUT_REPORT,
				 HID_REQ_SET_REPORT);

	if (ret < 0)
		hid_err(hdev, "hw_raw_request failed (command 0x%02x)\n",
			command);

	kfree(buf);
	return ret;
}

static void hyperx_headset_poll_work(struct work_struct *work)
{
	struct hyperx_headset_device *drvdata = container_of(
		work, struct hyperx_headset_device, poll_work.work);

	hyperx_headset_send_command(drvdata, HYPERX_CMD_GET_CONNECTED);
	hyperx_headset_send_command(drvdata, HYPERX_CMD_GET_BATTERY);
	hyperx_headset_send_command(drvdata, HYPERX_CMD_GET_CHARGING);

	schedule_delayed_work(&drvdata->poll_work,
			      msecs_to_jiffies(HYPERX_POLL_INTERVAL_MS));
}

static void hyperx_headset_set_wireless_status(struct hid_device *hdev,
					       bool connected)
{
	struct usb_interface *intf;

	if (!hid_is_usb(hdev))
		return;

	intf = to_usb_interface(hdev->dev.parent);
	usb_set_wireless_status(intf, connected ?
					      USB_WIRELESS_STATUS_CONNECTED :
					      USB_WIRELESS_STATUS_DISCONNECTED);
}

static void hyperx_headset_battery_work(struct work_struct *work)
{
	struct hyperx_headset_device *drvdata =
		container_of(work, struct hyperx_headset_device, battery_work);
	struct power_supply_config battery_cfg = { .drv_data = drvdata };
	unsigned long flags;
	bool connected;

	spin_lock_irqsave(&drvdata->lock, flags);
	connected = drvdata->is_connected;
	spin_unlock_irqrestore(&drvdata->lock, flags);

	hyperx_headset_set_wireless_status(drvdata->hdev, connected);

	if (connected && !drvdata->battery) {
		struct power_supply *ps;

		ps = power_supply_register(&drvdata->hdev->dev,
					   &hyperx_headset_battery_desc,
					   &battery_cfg);
		if (IS_ERR(ps)) {
			hid_err(drvdata->hdev,
				"power_supply_register failed\n");
			return;
		}

		power_supply_powers(ps, &drvdata->hdev->dev);

		spin_lock_irqsave(&drvdata->lock, flags);
		drvdata->battery = ps;
		spin_unlock_irqrestore(&drvdata->lock, flags);
	} else if (!connected && drvdata->battery) {
		struct power_supply *ps;

		spin_lock_irqsave(&drvdata->lock, flags);
		ps = drvdata->battery;
		drvdata->battery = NULL;
		spin_unlock_irqrestore(&drvdata->lock, flags);

		power_supply_unregister(ps);
	}
}

static void
hyperx_headset_parse_battery_event(struct hyperx_headset_device *drvdata,
				   u8 *data)
{
	unsigned long flags;
	u8 state1 = data[2];
	u8 state2 = data[3];
	u8 level = data[4];

	// Battery event is invalid if both states are 0
	if (state1 == 0 && state2 == 0)
		return;

	spin_lock_irqsave(&drvdata->lock, flags);

	if (drvdata->battery_level != level) {
		drvdata->battery_level = level;

		if (drvdata->battery)
			power_supply_changed(drvdata->battery);
	}

	spin_unlock_irqrestore(&drvdata->lock, flags);
}

static void
hyperx_headset_parse_charging_event(struct hyperx_headset_device *drvdata,
				    u8 *data)
{
	unsigned long flags;
	bool charging = (data[2] == 1);

	spin_lock_irqsave(&drvdata->lock, flags);

	if (drvdata->is_charging != charging) {
		drvdata->is_charging = charging;

		if (drvdata->battery)
			power_supply_changed(drvdata->battery);
	}

	spin_unlock_irqrestore(&drvdata->lock, flags);
}

static void
hyperx_headset_parse_connected_event(struct hyperx_headset_device *drvdata,
				     u8 *data)
{
	unsigned long flags;
	bool state_changed = false;
	bool connected = (data[2] == 1);

	spin_lock_irqsave(&drvdata->lock, flags);

	if (drvdata->is_connected != connected) {
		drvdata->is_connected = connected;
		state_changed = true;
	}

	spin_unlock_irqrestore(&drvdata->lock, flags);

	if (state_changed)
		schedule_work(&drvdata->battery_work);
}

static int hyperx_headset_probe(struct hid_device *hdev,
				const struct hid_device_id *id)
{
	int ret;
	struct hyperx_headset_device *drvdata;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (drvdata == NULL)
		return -ENOMEM;
	drvdata->hdev = hdev;
	drvdata->is_connected = false;
	drvdata->is_charging = false;
	drvdata->battery_level = 100;
	spin_lock_init(&drvdata->lock);
	hid_set_drvdata(hdev, drvdata);

	INIT_DELAYED_WORK(&drvdata->poll_work, hyperx_headset_poll_work);
	INIT_WORK(&drvdata->battery_work, hyperx_headset_battery_work);

	ret = hid_parse(hdev);
	if (ret != 0) {
		hid_err(hdev, "parse failed\n");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret != 0) {
		hid_err(hdev, "hw_start failed\n");
		return ret;
	}

	schedule_delayed_work(&drvdata->poll_work, 0);

	return 0;
}

static int hyperx_headset_raw_event(struct hid_device *hdev,
				    struct hid_report *report, u8 *data,
				    int size)
{
	struct hyperx_headset_device *drvdata = hid_get_drvdata(hdev);

	if (size < 5 || data[0] != HYPERX_REPORT_ID)
		return 0;

	switch (data[1]) {
	case HYPERX_CMD_GET_CONNECTED:
	case HYPERX_RESP_CONNECTED:
		hyperx_headset_parse_connected_event(drvdata, data);
		break;

	case HYPERX_CMD_GET_BATTERY:
	case HYPERX_RESP_BATTERY:
		hyperx_headset_parse_battery_event(drvdata, data);
		break;

	case HYPERX_CMD_GET_CHARGING:
	case HYPERX_RESP_CHARGING:
		hyperx_headset_parse_charging_event(drvdata, data);
		break;

	default:
		break;
	}

	return 0;
}

static void hyperx_headset_remove(struct hid_device *hdev)
{
	struct hyperx_headset_device *drvdata = hid_get_drvdata(hdev);

	if (drvdata) {
		cancel_delayed_work_sync(&drvdata->poll_work);
		cancel_work_sync(&drvdata->battery_work);

		if (drvdata->battery) {
			power_supply_unregister(drvdata->battery);
			drvdata->battery = NULL;
		}
	}

	hid_hw_stop(hdev);
}

static const struct hid_device_id hyperx_headset_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_HP,
			 USB_DEVICE_ID_HP_HYPERX_CLOUD_III_WIRELESS) },
	{}
};
MODULE_DEVICE_TABLE(hid, hyperx_headset_devices);

static struct hid_driver hyperx_headset_driver = {
	.name = "hyperx-headset",
	.id_table = hyperx_headset_devices,
	.probe = hyperx_headset_probe,
	.raw_event = hyperx_headset_raw_event,
	.remove = hyperx_headset_remove,
};
module_hid_driver(hyperx_headset_driver);

MODULE_AUTHOR("Sofia Schneider <sofia@schn.dev>");
MODULE_DESCRIPTION("HID driver for HyperX headsets");
MODULE_LICENSE("GPL");
