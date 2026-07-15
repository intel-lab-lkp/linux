// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Gigabyte Aero laptop vendor-specific brightness keys.
 *
 *  The keyboard sends brightness up/down presses as a vendor-defined usage page
 *  report instead of standard HID Consumer Control usages.
 *
 *  This driver intercepts them and emits the correct KEY_BRIGHTNESSUP /
 *  KEY_BRIGHTNESSDOWN events.
 *
 *  Currently supported devices are:
 *	Gigabyte Aero 15 XB
 *
 *  Copyright (c) 2026 Uddhav Swami <uddhavswami@gmail.com>
 *
 *  This module based on hid-asus by
 *  Copyright (c) 2016 Yusuke Fujimaki <usk.fujimaki@gmail.com>
 *  Copyright (c) 2016 Brendan McGrath <redmcg@redmandi.dyndns.org>
 *  Copyright (c) 2016 Victor Vlasenko <victor.vlasenko@sysgears.com>
 *  Copyright (c) 2016 Frederik Wenigwieser <frederik.wenigwieser@gmail.com>
 */

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/input.h>

#include "hid-ids.h"

MODULE_AUTHOR("Uddhav Swami <uddhavswami@gmail.com>");
MODULE_DESCRIPTION("HID driver for Gigabyte Aero");

#define GIGABYTE_AERO_REPORT_ID 0x04
#define GIGABYTE_AERO_BRIGHTNESS_DOWN 0x7D
#define GIGABYTE_AERO_BRIGHTNESS_UP 0x7E

struct gigabyte_drvdata {
	struct input_dev *input;
};

static int gigabyte_aero_raw_event(struct hid_device *hdev,
				   struct hid_report *report, u8 *data,
				   int size)
{
	struct gigabyte_drvdata *drvdata = hid_get_drvdata(hdev);

	if (!drvdata->input)
		return 0;

	if (report->id != GIGABYTE_AERO_REPORT_ID || size < 4)
		return 0;

	switch (data[3]) {
	case GIGABYTE_AERO_BRIGHTNESS_DOWN:
		input_report_key(drvdata->input, KEY_BRIGHTNESSDOWN, 1);
		input_sync(drvdata->input);
		input_report_key(drvdata->input, KEY_BRIGHTNESSDOWN, 0);
		input_sync(drvdata->input);
		return 1;
	case GIGABYTE_AERO_BRIGHTNESS_UP:
		input_report_key(drvdata->input, KEY_BRIGHTNESSUP, 1);
		input_sync(drvdata->input);
		input_report_key(drvdata->input, KEY_BRIGHTNESSUP, 0);
		input_sync(drvdata->input);
		return 1;
	default:
		return 0;
	}
}

static int gigabyte_aero_input_configured(struct hid_device *hdev,
					  struct hid_input *hi)
{
	struct gigabyte_drvdata *drvdata = hid_get_drvdata(hdev);
	struct hid_report *report;
	bool has_report = false;

	list_for_each_entry(report, &hi->reports, hidinput_list) {
		if (report->id == GIGABYTE_AERO_REPORT_ID) {
			has_report = true;
			break;
		}
	}

	if (!has_report)
		return 0;

	input_set_capability(hi->input, EV_KEY, KEY_BRIGHTNESSUP);
	input_set_capability(hi->input, EV_KEY, KEY_BRIGHTNESSDOWN);

	drvdata->input = hi->input;

	return 0;
}

static int gigabyte_aero_probe(struct hid_device *hdev,
			       const struct hid_device_id *id)
{
	struct gigabyte_drvdata *drvdata;
	int error;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	hid_set_drvdata(hdev, drvdata);

	error = hid_parse(hdev);
	if (error) {
		hid_err(hdev, "gigabyte_aero: parse failed: %d\n", error);
		return error;
	}

	error = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (error) {
		hid_err(hdev, "gigabyte_aero: hw start failed: %d\n", error);
		return error;
	}

	if (!(hdev->claimed & HID_CLAIMED_INPUT)) {
		hid_err(hdev, "gigabyte_aero: no input device claimed\n");
		hid_hw_stop(hdev);
		return -ENODEV;
	}

	return 0;
}

static const struct hid_device_id gigabyte_aero_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_CHU_YUEN,
			 USB_DEVICE_ID_CHU_YUEN_AERO_KBD) },
	{}
};
MODULE_DEVICE_TABLE(hid, gigabyte_aero_devices);

static struct hid_driver gigabyte_aero_driver = {
	.name = "hid_gigabyte_aero",
	.id_table = gigabyte_aero_devices,
	.probe = gigabyte_aero_probe,
	.raw_event = gigabyte_aero_raw_event,
	.input_configured = gigabyte_aero_input_configured,
};
module_hid_driver(gigabyte_aero_driver);

MODULE_LICENSE("GPL");
