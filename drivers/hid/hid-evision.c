// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for EVision devices
 *  For now, only ignore bogus consumer reports
 *  sent after the keyboard has been configured
 *
 *  Copyright (c) 2022 Philippe Valembois
 */

#include <linux/device.h>
#include <linux/input.h>
#include <linux/hid.h>
#include <linux/module.h>

#include "hid-ids.h"

static int evision_k552_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	if ((usage->hid & HID_USAGE_PAGE) != HID_UP_CONSUMER)
		return 0;

	switch (usage->hid & HID_USAGE) {
	/* report 3 */
	case 0x183:
		hid_map_usage_clear(hi, usage, bit, max, EV_KEY, KEY_MEDIA);
		break;
	default:
		return 0;
	}

	return 1;
}

static int evision_icl01_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	if ((usage->hid & HID_USAGE_PAGE) != HID_UP_CONSUMER)
		return 0;

	/* Ignore key down event */
	if ((usage->hid & HID_USAGE) >> 8 == 0x05)
		return -1;
	/* Ignore key up event */
	if ((usage->hid & HID_USAGE) >> 8 == 0x06)
		return -1;

	switch (usage->hid & HID_USAGE) {
	/* Ignore configuration saved event */
	case 0x0401: return -1;
	/* Ignore reset event */
	case 0x0402: return -1;
	}
	return 0;
}

static int evision_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	int ret = 0;

	if (hdev->product == USB_DEVICE_ID_EVISION_ICL01) {
		ret = evision_icl01_input_mapping(hdev, hi,
			field, usage, bit, max);
	} else if (hdev->product == USB_DEVICE_ID_EVISION_K552) {
		ret = evision_k552_input_mapping(hdev, hi,
			field, usage, bit, max);
	}

	return ret;
}

#define REP_DSC_SIZE 236
#define USAGE_MAX_INDEX 59

static const __u8 *evision_report_fixup(struct hid_device *hdev, __u8 *rdesc,
		unsigned int *rsize)
{
	if (hdev->product == USB_DEVICE_ID_EV_TELINK_RECEIVER &&
	    *rsize == REP_DSC_SIZE && rdesc[USAGE_MAX_INDEX] == 0x29 &&
	    rdesc[USAGE_MAX_INDEX + 1] == 3) {
		hid_info(hdev, "fixing EVision:TeLink Receiver report descriptor\n");
		rdesc[USAGE_MAX_INDEX + 1] = 5; // change usage max from 3 to 5
	}
	return rdesc;
}

static const struct hid_device_id evision_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_EVISION, USB_DEVICE_ID_EVISION_ICL01) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_EVISION, USB_DEVICE_ID_EV_TELINK_RECEIVER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_EVISION, USB_DEVICE_ID_EVISION_K552) },
	{ }
};
MODULE_DEVICE_TABLE(hid, evision_devices);

static struct hid_driver evision_driver = {
	.name = "evision",
	.id_table = evision_devices,
	.input_mapping = evision_input_mapping,
	.report_fixup = evision_report_fixup,
};
module_hid_driver(evision_driver);

MODULE_DESCRIPTION("HID driver for EVision devices");
MODULE_LICENSE("GPL");
