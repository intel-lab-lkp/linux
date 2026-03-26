// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * witrn - Driver for WITRN USB charging testers
 *
 * Copyright (C) 2026 Rong Zhang <i@rong.moe>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/units.h>
#include <linux/workqueue.h>

#define DRIVER_NAME		"witrn"
#define WITRN_EP_CMD_OUT	0x01
#define WITRN_EP_DATA_IN	0x81

#define WITRN_REPORT_SZ		64

/* flags */
#define WITRN_HID_OPENED	0

/*
 * The device sends reports every 10ms (100Hz!) once it's opened, which is
 * really annoying and produces a lot of irq noise.
 *
 * Unfortunately, the device doesn't provide any command to start/stop reporting
 * on demand -- it simply spams reports blindly. The only way to stop reporting
 * is to close the HID device (i.e., to stop IN URB (re)submission).
 *
 * Let's close the HID device if the device has not been accessed for a while.
 */
#define PAUSE_TIMEOUT		secs_to_jiffies(8)
#define UP_TO_DATE_TIMEOUT	msecs_to_jiffies(100)

enum witrn_report_type {
	WITRN_PD		= 0xfe,
	WITRN_SENSOR		= 0xff,
};

struct witrn_sensor {
	__le16	record_threshold;	/* mA */
	__le32	record_charge;		/* Ah (float) */
	__le32	record_energy;		/* Wh (float) */
	__le32	record_time;		/* s */
	__le32	uptime;			/* s */
	__le32	vdp;			/* V (float) */
	__le32	vdm;			/* V (float) */
	u8	__unknown[4];
	__le32	temp_ntc;		/* Celsius (float) */
	__le32	vbus;			/* V (float) */
	__le32	ibus;			/* A (float) */
	u8	record_group;		/* 0: group 1 on device, ... */
	u8	vcc1;			/* dV */
	u8	vcc2;			/* dV */
} __packed;

struct witrn_report {
	u8	report_type;
	u8	__unknown_0[11];

	struct witrn_sensor sensor;

	u8	__unknown_1[7];
} __packed;
static_assert(sizeof(struct witrn_report) == WITRN_REPORT_SZ);

struct witrn_priv {
	struct hid_device *hdev;

	struct work_struct pause_work;

	unsigned long flags;

	spinlock_t lock; /* Protects members below */

	struct completion completion;
	unsigned long last_update; /* jiffies */
	unsigned long last_access; /* jiffies */

	struct witrn_sensor sensor;
};

static inline bool sensor_is_outdated(struct witrn_priv *priv)
{
	return time_after(jiffies, priv->last_update + UP_TO_DATE_TIMEOUT);
}

static inline bool hwmon_is_inactive(struct witrn_priv *priv)
{
	return time_after(jiffies, priv->last_access + PAUSE_TIMEOUT);
}

/* ======== HID ======== */

static int witrn_open_hid(struct witrn_priv *priv)
{
	int ret;

	if (test_and_set_bit(WITRN_HID_OPENED, &priv->flags))
		return 0; /* Already opened */

	hid_dbg(priv->hdev, "opening hid hw\n");

	ret = hid_hw_open(priv->hdev);
	if (ret) {
		hid_err(priv->hdev, "hid hw open failed with %d\n", ret);
		clear_bit(WITRN_HID_OPENED, &priv->flags);
	}

	return ret;
}

static void witrn_close_hid(struct witrn_priv *priv)
{
	if (!test_and_clear_bit(WITRN_HID_OPENED, &priv->flags))
		return; /* Already closed */

	hid_dbg(priv->hdev, "closing hid hw\n");

	hid_hw_close(priv->hdev);
}

static void witrn_pause_hid(struct work_struct *work)
{
	struct witrn_priv *priv = container_of(work, struct witrn_priv, pause_work);

	scoped_guard(spinlock, &priv->lock) {
		/* Double check. Condition may change after being scheduled. */
		if (!hwmon_is_inactive(priv))
			return;
	}

	witrn_close_hid(priv);
}

static int witrn_raw_event(struct hid_device *hdev, struct hid_report *report,
			   u8 *data, int size)
{
	struct witrn_priv *priv = hid_get_drvdata(hdev);
	const struct witrn_report *wreport;
	bool do_pause = false;

	/* HIDRAW has opened the device while we are pausing. */
	if (!test_bit(WITRN_HID_OPENED, &priv->flags))
		return 0;

	if (size < WITRN_REPORT_SZ) {
		hid_dbg(hdev, "report size mismatch: %d < %d\n", size, WITRN_REPORT_SZ);
		return 0;
	}

	wreport = (const struct witrn_report *)data;
	if (wreport->report_type != WITRN_SENSOR) {
		hid_dbg(hdev, "report ignored with type 0x%02x", wreport->report_type);
		return 0;
	}

	scoped_guard(spinlock, &priv->lock) {
		priv->last_update = jiffies;
		do_pause = hwmon_is_inactive(priv);

		memcpy(&priv->sensor, &wreport->sensor, sizeof(wreport->sensor));
		complete(&priv->completion);
	}

	if (do_pause)
		schedule_work(&priv->pause_work);

	return 0;
}

static int witrn_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct device *parent = &hdev->dev;
	struct witrn_priv *priv;
	int ret;

	priv = devm_kzalloc(parent, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed with %d\n", ret);
		return ret;
	}

	/* Enable HIDRAW so existing user-space tools can continue to work. */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid hw start failed with %d\n", ret);
		return ret;
	}

	spin_lock_init(&priv->lock);
	init_completion(&priv->completion);

	INIT_WORK(&priv->pause_work, witrn_pause_hid);

	priv->last_access = jiffies;
	priv->last_update = priv->last_access - UP_TO_DATE_TIMEOUT - 1;
	clear_bit(WITRN_HID_OPENED, &priv->flags);

	ret = witrn_open_hid(priv);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}

	return 0;
}

static void witrn_remove(struct hid_device *hdev)
{
	struct witrn_priv *priv = hid_get_drvdata(hdev);

	witrn_close_hid(priv);

	/* Cancel it after closing HID so that it won't be rescheduled. */
	cancel_work_sync(&priv->pause_work);

	hid_hw_stop(hdev);
}

static const struct hid_device_id witrn_id_table[] = {
	{ HID_USB_DEVICE(0x0716, 0x5060) },	/* WITRN K2 USB-C tester */
	{ }
};

MODULE_DEVICE_TABLE(hid, witrn_id_table);

static struct hid_driver witrn_driver = {
	.name = DRIVER_NAME,
	.id_table = witrn_id_table,
	.probe = witrn_probe,
	.remove = witrn_remove,
	.raw_event = witrn_raw_event,
};

static int __init witrn_init(void)
{
	return hid_register_driver(&witrn_driver);
}

static void __exit witrn_exit(void)
{
	hid_unregister_driver(&witrn_driver);
}

/* When compiled into the kernel, initialize after the HID bus */
late_initcall(witrn_init);
module_exit(witrn_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rong Zhang <i@rong.moe>");
MODULE_DESCRIPTION("WITRN USB tester driver");
