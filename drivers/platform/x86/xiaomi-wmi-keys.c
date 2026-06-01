// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi laptop WMI hotkey driver
 *
 * Copyright (C) 2026 Lucy Wong <lucywong2778@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/wmi.h>

#define XIAOMI_WMI_EVENT_GUID "B74AF83F-8B2F-4069-ACAC-36D176F62FC0"

#define XIAOMI_TYPE_BACKLIGHT 0x01 /* keyboard backlight level (EC handled) */
#define XIAOMI_TYPE_KEY  0x02 /* press/release keys */
#define XIAOMI_TYPE_TOGGLE 0x03 /* firmware toggle keys (notify only) */

#define XIAOMI_KEY_PRESS_MIN 0x01
#define XIAOMI_KEY_PRESS_MAX 0x05
#define XIAOMI_KEY_RELEASE_OFF 0x05

#define XIAOMI_TOGGLE_DEDUP_MS 50

struct xiaomi_wmi {
	struct input_dev *input_dev;
	unsigned long last_toggle;
	u8 last_toggle_code;
};

static const struct key_entry xiaomi_wmi_keymap[] = {
	/* type 0x02: programmable keys with abstract icons */
	{ KE_KEY, 0x0201, { KEY_MACRO1 } },
	{ KE_KEY, 0x0202, { KEY_MACRO2 } },
	{ KE_KEY, 0x0203, { KEY_MACRO3 } },
	{ KE_KEY, 0x0204, { KEY_MACRO4 } },
	{ KE_KEY, 0x0205, { KEY_MACRO5 } },
	/* type 0x03: firmware-handled toggles, reported for OSD only */
	{ KE_KEY, 0x0300, { KEY_F21 } }, /* touchpad toggle */
	{ KE_KEY, 0x0301, { KEY_FN_ESC } }, /* Fn lock */
	{ KE_END, 0 },
};

static int xiaomi_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct xiaomi_wmi *data;
	int ret;

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	dev_set_drvdata(&wdev->dev, data);

	data->input_dev = devm_input_allocate_device(&wdev->dev);
	if (!data->input_dev)
		return -ENOMEM;

	data->input_dev->name = "Xiaomi WMI keys";
	data->input_dev->phys = "wmi/input0";
	data->input_dev->id.bustype = BUS_HOST;

	ret = sparse_keymap_setup(data->input_dev, xiaomi_wmi_keymap, NULL);
	if (ret)
		return ret;

	return input_register_device(data->input_dev);
}

static void xiaomi_wmi_notify(struct wmi_device *wdev, union acpi_object *obj)
{
	struct xiaomi_wmi *data = dev_get_drvdata(&wdev->dev);
	u8 type, code;

	if (obj->type != ACPI_TYPE_BUFFER || obj->buffer.length < 3)
		return;

	type = obj->buffer.pointer[1];
	code = obj->buffer.pointer[2];

	switch (type) {
	case XIAOMI_TYPE_KEY:
		if (code >= XIAOMI_KEY_PRESS_MIN &&
		    code <= XIAOMI_KEY_PRESS_MAX) {
			sparse_keymap_report_event(data->input_dev,
						   0x0200 | code, 1, false);
		} else if (code > XIAOMI_KEY_PRESS_MAX &&
						code <= XIAOMI_KEY_PRESS_MAX +
							XIAOMI_KEY_RELEASE_OFF) {
			code -= XIAOMI_KEY_RELEASE_OFF;
			sparse_keymap_report_event(data->input_dev,
						   0x0200 | code, 0, false);
		}
		break;

	case XIAOMI_TYPE_TOGGLE:
		/* deduplicate: each press emits two identical events */
		if (data->last_toggle_code == code &&
		    time_before(jiffies, data->last_toggle +
				msecs_to_jiffies(XIAOMI_TOGGLE_DEDUP_MS)))
			return;
		data->last_toggle_code = code;
		data->last_toggle = jiffies;
		sparse_keymap_report_event(data->input_dev,
					   0x0300 | code, 1, true);
		break;

	case XIAOMI_TYPE_BACKLIGHT:
		/* keyboard backlight handled by EC, nothing to do */
		break;

	default:
		dev_dbg(&wdev->dev, "unknown event type 0x%02x\n", type);
		break;
	}
}

static const struct wmi_device_id xiaomi_wmi_id_table[] = {
	{ .guid_string = XIAOMI_WMI_EVENT_GUID },
	{ }
};
MODULE_DEVICE_TABLE(wmi, xiaomi_wmi_id_table);

static struct wmi_driver xiaomi_wmi_driver = {
	.driver = {
		.name = "xiaomi-wmi-keys",
	},
	.id_table = xiaomi_wmi_id_table,
	.probe = xiaomi_wmi_probe,
	.notify = xiaomi_wmi_notify,
	.no_singleton = true,
};
module_wmi_driver(xiaomi_wmi_driver);

MODULE_AUTHOR("Lucy Wong <lucywong2778@gmail.com>");
MODULE_DESCRIPTION("Xiaomi laptop WMI hotkey driver");
MODULE_LICENSE("GPL");
