// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID UNIVERSAL PIDFF
 * hid-pidff wrapper for PID-enabled devices
 * Handles device reports, quirks and extends usable button range
 *
 * Copyright (c) 2024 Makarenko Oleg
 * Copyright (c) 2024 Tomasz Pakuła
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/input-event-codes.h>
#include "hid-ids.h"

#define JOY_RANGE (BTN_DEAD - BTN_JOYSTICK + 1)

static const u8 *moza_report_fixup(struct hid_device *hdev, __u8 *rdesc, unsigned int *rsize)
{
	// Fix data type on PID Device Control
	if (rdesc[1002] == 0x91 && rdesc[1003] == 0x02) {
		rdesc[1003] = 0x00; // Fix header, it needs to be Array.
	}
	return rdesc;
}


static const u8 *universal_pidff_report_fixup(struct hid_device *hdev, __u8 *rdesc,
					      unsigned int *rsize)
{
	if (hdev->vendor == USB_VENDOR_ID_MOZA) {
		return moza_report_fixup(hdev, rdesc, rsize);
	}
	return rdesc;
}

/*
 * Map buttons manually to extend the default joystick buttn limit
 */
static int universal_pidff_input_mapping(struct hid_device *hdev,
	struct hid_input *hi, struct hid_field *field, struct hid_usage *usage,
	unsigned long **bit, int *max)
{
	// Let the default behavior handle mapping if usage is not a button
	if ((usage->hid & HID_USAGE_PAGE) != HID_UP_BUTTON)
		return 0;

	int button = ((usage->hid - 1) & HID_USAGE);
	int code = button + BTN_JOYSTICK;

	// Detect the end of JOYSTICK buttons range
	// KEY_NEXT_FAVORITE = 0x270
	if (code > BTN_DEAD)
		code = button + KEY_NEXT_FAVORITE - JOY_RANGE;

	// Map overflowing buttons to KEY_RESERVED to not ignore
	// them and let them still trigger MSC_SCAN
	if (code > KEY_MAX)
		code = KEY_RESERVED;

	hid_map_usage(hi, usage, bit, max, EV_KEY, code);
	hid_dbg(hdev, "Button %d: usage %d", button, code);
	return 1;
}


/*
 * Check if the device is PID and initialize it
 * Add quirks after initialisation
 */
static int universal_pidff_probe(struct hid_device *hdev,
				 const struct hid_device_id *id)
{
	int error;
	error = hid_parse(hdev);
	if (error) {
		hid_err(hdev, "HID parse failed\n");
		goto err;
	}

	error = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
	if (error) {
		hid_err(hdev, "HID hw start failed\n");
		goto err;
	}

	u32 quirks = id->driver_data;
	error = hid_pidff_init_with_quirks(hdev, quirks);
	if (error) {
		hid_warn(hdev, "Force Feedback initialization failed\n");
		goto err;
	}

	hid_info(hdev, "Universal pidff driver loaded sucesfully!");

	return 0;
err:
	return error;
}

static int universal_pidff_input_configured(struct hid_device *hdev,
					    struct hid_input *hidinput)
{
	// Remove fuzz and deadzone from the wheel/joystick axis
	struct input_dev *input = hidinput->input;
	input_set_abs_params(input, ABS_X,
		input->absinfo[ABS_X].minimum, input->absinfo[ABS_X].maximum, 0, 0);

	// Decrease fuzz and deadzone on additional axes
	// Default Linux values are 255 for fuzz and 4096 for flat (deadzone)
	int axis;
	for (axis = ABS_Y; axis <= ABS_BRAKE; axis++) {
		if (!test_bit(axis, input->absbit))
			continue;

		input_set_abs_params(input, axis,
			input->absinfo[axis].minimum,
			input->absinfo[axis].maximum, 8, 0);
	}

	// Remove fuzz and deadzone from the second joystick axis
	if (hdev->vendor == USB_VENDOR_ID_FFBEAST &&
	    hdev->product == USB_DEVICE_ID_FFBEAST_JOYSTICK)
		input_set_abs_params(input, ABS_X,
			input->absinfo[ABS_X].minimum,
			input->absinfo[ABS_X].maximum, 0, 0);

	return 0;
}

static const struct hid_device_id universal_pidff_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R3),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R5),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R9),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R12),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R16_R21),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R3_ALT),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R5_ALT),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R9_ALT),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R12_ALT),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MOZA, USB_DEVICE_ID_MOZA_R16_R21_ALT),
		.driver_data = HID_PIDFF_QUIRK_FIX_WHEEL_DIRECTION },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CAMMUS, USB_DEVICE_ID_CAMMUS_C5) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CAMMUS, USB_DEVICE_ID_CAMMUS_C12) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_VRS, USB_DEVICE_ID_VRS_DFP),
		.driver_data = HID_PIDFF_QUIRK_MISSING_DEVICE_CONTROL },
	{ HID_USB_DEVICE(USB_VENDOR_ID_FFBEAST, USB_DEVICE_ID_FFBEAST_JOYSTICK), },
	{ HID_USB_DEVICE(USB_VENDOR_ID_FFBEAST, USB_DEVICE_ID_FFBEAST_RUDDER), },
	{ HID_USB_DEVICE(USB_VENDOR_ID_FFBEAST, USB_DEVICE_ID_FFBEAST_WHEEL) },
	{ }
};
MODULE_DEVICE_TABLE(hid, universal_pidff_devices);

static struct hid_driver universal_pidff = {
	.name = "hid-universal-pidff",
	.id_table = universal_pidff_devices,
	.input_mapping = universal_pidff_input_mapping,
	.probe = universal_pidff_probe,
	.input_configured = universal_pidff_input_configured,
	.report_fixup = universal_pidff_report_fixup
};
module_hid_driver(universal_pidff);

MODULE_DESCRIPTION("Universal driver for PID Force Feedback devices");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Makarenko Oleg <oleg@makarenk.ooo>");
MODULE_AUTHOR("Tomasz Pakuła <tomasz.pakula.oficjalny@gmail.com>");
