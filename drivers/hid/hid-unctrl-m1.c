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

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "hid-ids.h"

#define UNCTRL_VENDOR_IFNUM 2

#define UNCTRL_BATTERY_TIMEOUT_MS 5000

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
	struct list_head list;
	struct hid_device *hdev;

	struct power_supply_desc battery_desc;
	struct power_supply *battery;

	/* raw_event runs in the completion path, so this is a spinlock. */
	spinlock_t lock;

	struct delayed_work battery_work;

	u8 battery_capacity;
	u8 mode;
	bool online;
	bool wired;
	bool vendor;
};

static LIST_HEAD(unctrl_connections);
static DEFINE_MUTEX(unctrl_lock);

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

	scoped_guard(spinlock_irqsave, &dev->lock) {
		capacity = dev->battery_capacity;
		mode = dev->mode;
		online = dev->online;
	}

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

static void unctrl_battery_timer_tick(struct work_struct *work)
{
	struct unctrl_dev *dev =
		container_of(work, struct unctrl_dev, battery_work.work);

	unctrl_fetch_battery(dev->hdev);

	queue_delayed_work(system_freezable_wq, &dev->battery_work,
			      msecs_to_jiffies(UNCTRL_BATTERY_TIMEOUT_MS));
}

static struct unctrl_dev *unctrl_battery_owner(void)
{
	struct unctrl_dev *dev;

	list_for_each_entry(dev, &unctrl_connections, list) {
		if (dev->battery)
			return dev;
	}

	return NULL;
}

/*
 * Register the power supply on the connection the mouse talks over,
 * a wired connection takes it away from a receiver. Called with
 * unctrl_lock held.
 */
static int unctrl_battery_register(struct unctrl_dev *dev)
{
	struct power_supply_config config = { .drv_data = dev };
	struct unctrl_dev *owner;
	struct power_supply *battery;
	int ret;

	owner = unctrl_battery_owner();
	if (owner == dev || (owner && (!dev->wired || owner->wired)))
		return 0;

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
	scoped_guard(spinlock_irqsave, &dev->lock)
		dev->battery = battery;
	hid_info(dev->hdev, "%s:battery registered as %s\n", __func__,
		 dev->battery_desc.name);

	/* the takeover cannot fail now, our supply already exists */
	if (owner) {
		struct power_supply *owner_battery;

		cancel_delayed_work_sync(&owner->battery_work);
		/* so a raw_event cannot touch the supply being unregistered */
		scoped_guard(spinlock_irqsave, &owner->lock) {
			owner_battery = owner->battery;
			owner->battery = NULL;
		}
		power_supply_unregister(owner_battery);
	}

	unctrl_fetch_battery(dev->hdev);
	queue_delayed_work(system_freezable_wq, &dev->battery_work,
			      msecs_to_jiffies(UNCTRL_BATTERY_TIMEOUT_MS));

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
	INIT_DELAYED_WORK(&dev->battery_work, unctrl_battery_timer_tick);

	dev->battery_capacity = 100;
	dev->mode = UNCTRL_MODE_RECEIVER;
	dev->wired = hdev->product == USB_DEVICE_ID_UNCTRL_M1_WIRED;
	dev->vendor = vendor;
	spin_lock_init(&dev->lock);

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

	/* the reply to the probe-time query may arrive before probe
	 * returns, when the core still gates the input path
	 */
	hid_device_io_start(hdev);

	hid_set_drvdata(hdev, dev);

	/* the list membership, the registration and any failure cleanup are
	 * one step under unctrl_lock, so a concurrent handover cannot hand
	 * this probe a battery while it is failing out
	 */
	mutex_lock(&unctrl_lock);
	list_add_tail(&dev->list, &unctrl_connections);
	ret = unctrl_battery_register(dev);
	if (ret)
		list_del(&dev->list);
	mutex_unlock(&unctrl_lock);

	if (ret) {
		hid_hw_close(hdev);
		hid_hw_stop(hdev);
		hid_set_drvdata(hdev, NULL);
		kfree(dev);
		return ret;
	}

	return 0;
}

static int unctrl_raw_event(struct hid_device *hdev, struct hid_report *report,
			    u8 *data, int size)
{
	struct unctrl_dev *dev = hid_get_drvdata(hdev);
	struct power_supply *battery = NULL;
	bool changed;
	u8 old_capacity, old_mode;

	if (!dev || size < 8)
		return 0;

	if (data[0] != 0x0A || data[1] != 0xAA || data[2] != 0xFF ||
	    data[3] != 0xDD)
		return 0;

	/* real answers carry the mode here, echoes of the query do not */
	if (data[5] != UNCTRL_MODE_RECEIVER && data[5] != UNCTRL_MODE_WIRED)
		return 0;

	scoped_guard(spinlock_irqsave, &dev->lock) {
		/* the receiver reports 0% while the mouse is switched off */
		old_capacity = dev->battery_capacity;
		old_mode = dev->mode;

		dev->battery_capacity = min_t(u8, data[4], 100);
		dev->mode = data[5];
		dev->online = dev->battery_capacity != 0;

		changed = dev->battery_capacity != old_capacity ||
			  dev->mode != old_mode;

		battery = dev->battery;
		/*
		 * Only schedules a work, so safe from the completion path that
		 * raw_event runs in. Under the lock so that the unregistering side
		 * is either done or still blocked when we touch the pointer.
		 */
		if (battery && changed)
			power_supply_changed(battery);
	}

	return 0;
}

static void unctrl_remove(struct hid_device *hdev)
{
	struct unctrl_dev *dev = hid_get_drvdata(hdev);
	struct unctrl_dev *other;

	/* Stop polling before the transport goes down. */
	if (dev)
		hid_hw_close(hdev);

	hid_hw_stop(hdev);

	if (!dev)
		return;

	/*
	 * Pass the battery to a capable connection without one, so that it
	 * keeps being reported when the owner disappears.
	 */
	mutex_lock(&unctrl_lock);
	list_del(&dev->list);
	/* out of the list, nothing can reach this device anymore */
	cancel_delayed_work_sync(&dev->battery_work);
	if (dev->battery) {
		struct power_supply *battery;

		scoped_guard(spinlock_irqsave, &dev->lock) {
			battery = dev->battery;
			dev->battery = NULL;
		}
		power_supply_unregister(battery);
		list_for_each_entry(other, &unctrl_connections, list) {
			if (!other->vendor || other->battery)
				continue;
			unctrl_battery_register(other);
			break;
		}
	}
	mutex_unlock(&unctrl_lock);

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
