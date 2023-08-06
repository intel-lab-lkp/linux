// SPDX-License-Identifier: GPL-2.0
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/backlight.h>

#define APPLE_STUDIO_DISPLAY_VENDOR_ID  0x05ac
#define APPLE_STUDIO_DISPLAY_PRODUCT_ID 0x1114

#define HID_USAGE_MONITOR_CTRL			0x800001
#define HID_USAGE_VESA_VCP_BRIGHTNESS		0x820010

/*
 * Apple Studio Display HID report descriptor
 *
 * Usage Page (Monitor),               ; USB monitor (80h, monitor page)
 * Usage (01h),
 * Collection (Application),
 *     Report ID (1),
 *
 *     Usage Page (Monitor VESA VCP),  ; Monitor VESA virtual control panel (82h, monitor page)
 *     Usage (10h, Brightness),
 *     Logical Minimum (400),
 *     Logical Maximum (60000),
 *     Unit (Centimeter^-2 * Candela),
 *     Unit Exponent (14),
 *     Report Size (32),
 *     Report Count (1),
 *     Feature (Variable, Null State),
 *
 *     Usage Page (PID),               ; Physical interface device (0Fh)
 *     Usage (50h),
 *     Logical Minimum (0),
 *     Logical Maximum (20000),
 *     Unit (1001h),
 *     Unit Exponent (13),
 *     Report Size (16),
 *     Feature (Variable, Null State),
 *
 *     Usage Page (Monitor VESA VCP),  ; Monitor VESA virtual control panel (82h, monitor page)
 *     Usage (10h, Brightness),
 *     Logical Minimum (400),
 *     Logical Maximum (60000),
 *     Unit (Centimeter^-2 * Candela),
 *     Unit Exponent (14),
 *     Report Size (32),
 *     Report Count (1),
 *     Input (Variable),
 * End Collection
 */

struct hid_bl_data {
	struct hid_device *hdev;
	unsigned int min_brightness;
	unsigned int max_brightness;
	struct hid_field *input_field;
	struct hid_field *feature_field;
};

static struct hid_field *hid_get_usage_field(struct hid_device *hdev,
					     unsigned int report_type,
					     unsigned int application, unsigned int usage)
{
	struct hid_report_enum *re = &hdev->report_enum[report_type];
	struct hid_report *report;
	int i, j;

	list_for_each_entry(report, &re->report_list, list) {
		if (report->application != application)
			continue;

		for (i = 0; i < report->maxfield; i++) {
			struct hid_field *field = report->field[i];

			for (j = 0; j < field->maxusage; j++)
				if (field->usage[j].hid == usage)
					return field;
		}
	}
	return NULL;
}

static void hid_bl_remove(struct hid_device *hdev)
{
	struct backlight_device *bl;
	struct hid_bl_data *data;

	hid_dbg(hdev, "remove\n");
	bl = hid_get_drvdata(hdev);
	data = bl_get_data(bl);

	devm_backlight_device_unregister(&hdev->dev, bl);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	hid_set_drvdata(hdev, NULL);
	devm_kfree(&hdev->dev, data);
}

static int hid_get_brightness(struct hid_bl_data *data)
{
	struct hid_field *field;
	int result;

	field = data->input_field;
	hid_hw_request(data->hdev, field->report, HID_REQ_GET_REPORT);
	hid_hw_wait(data->hdev);
	result = *field->new_value;
	hid_dbg(data->hdev, "get brightness: %d\n", result);

	return result;
}

static int hid_bl_get_brightness(struct backlight_device *bl)
{
	struct hid_bl_data *data;
	int brightness;

	data = bl_get_data(bl);
	brightness = hid_get_brightness(data);
	return brightness - data->min_brightness;
}

static void hid_set_brightness(struct hid_bl_data *data, int brightness)
{
	struct hid_field *field;

	field = data->feature_field;
	*field->value = brightness;
	hid_hw_request(data->hdev, field->report, HID_REQ_SET_REPORT);
	hid_hw_wait(data->hdev);
	hid_dbg(data->hdev, "set brightness: %d\n", brightness);
}

static int hid_bl_update_status(struct backlight_device *bl)
{
	struct hid_bl_data *data;
	int brightness;

	data = bl_get_data(bl);
	brightness = backlight_get_brightness(bl);
	brightness += data->min_brightness;
	hid_set_brightness(data, brightness);
	return 0;
}

static const struct backlight_ops hid_bl_ops = {
	.update_status  = hid_bl_update_status,
	.get_brightness = hid_bl_get_brightness,
};

static int hid_bl_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct hid_field *input_field;
	struct hid_field *feature_field;
	struct hid_bl_data *data;
	struct backlight_properties props;
	struct backlight_device *bl;

	hid_dbg(hdev, "probe\n");

	ret = hid_parse(hdev);
	if (ret)
		hid_err(hdev, "parse failed\n");

	ret = hid_hw_start(hdev, HID_CONNECT_DRIVER);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		return ret;
	}

	input_field = hid_get_usage_field(hdev, HID_INPUT_REPORT,
					  HID_USAGE_MONITOR_CTRL,
					  HID_USAGE_VESA_VCP_BRIGHTNESS);
	if (input_field == NULL) {
		ret = -ENODEV;
		goto exit_stop;
	}

	feature_field = hid_get_usage_field(hdev, HID_FEATURE_REPORT,
					    HID_USAGE_MONITOR_CTRL,
					    HID_USAGE_VESA_VCP_BRIGHTNESS);
	if (feature_field == NULL) {
		ret = -ENODEV;
		goto exit_stop;
	}

	if (input_field->logical_minimum != feature_field->logical_minimum) {
		hid_warn(hdev, "minimums do not match: %d / %d\n",
			 input_field->logical_minimum,
			 feature_field->logical_minimum);
		ret = -ENODEV;
		goto exit_stop;
	}

	if (input_field->logical_maximum != feature_field->logical_maximum) {
		hid_warn(hdev, "maximums do not match: %d / %d\n",
			 input_field->logical_maximum,
			 feature_field->logical_maximum);
		ret = -ENODEV;
		goto exit_stop;
	}

	hid_dbg(hdev, "Monitor VESA VCP with brightness control\n");

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hw open failed\n");
		goto exit_stop;
	}

	data = devm_kzalloc(&hdev->dev, sizeof(struct hid_bl_data), GFP_KERNEL);
	if (data == NULL) {
		ret = -ENOMEM;
		goto exit_stop;
	}
	data->hdev = hdev;
	data->min_brightness = input_field->logical_minimum;
	data->max_brightness = input_field->logical_maximum;
	data->input_field = input_field;
	data->feature_field = feature_field;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = data->max_brightness - data->min_brightness;

	bl = devm_backlight_device_register(&hdev->dev, "vesa_vcp",
					    &hdev->dev, data,
					    &hid_bl_ops,
					    &props);
	if (IS_ERR(bl)) {
		hid_err(hdev, "failed to register backlight\n");
		ret = PTR_ERR(bl);
		goto exit_free;
	}

	hid_set_drvdata(hdev, bl);

	return 0;

exit_free:
	hid_hw_close(hdev);
	devm_kfree(&hdev->dev, data);

exit_stop:
	hid_hw_stop(hdev);
	return ret;
}

static const struct hid_device_id hid_bl_devices[] = {
	{ HID_USB_DEVICE(APPLE_STUDIO_DISPLAY_VENDOR_ID,
			 APPLE_STUDIO_DISPLAY_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, hid_bl_devices);

static struct hid_driver hid_bl_driver = {
	.name = "hid_backlight",
	.id_table = hid_bl_devices,
	.probe = hid_bl_probe,
	.remove = hid_bl_remove,
};
module_hid_driver(hid_bl_driver);

MODULE_AUTHOR("Julius Zint <julius@zint.sh>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Backlight driver for HID devices");
