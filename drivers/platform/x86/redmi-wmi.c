// SPDX-License-Identifier: GPL-2.0
/* WMI driver for Xiaomi Redmibooks */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/wmi.h>

#include <uapi/linux/input-event-codes.h>

#define WMI_REDMIBOOK_KEYBOARD_EVENT_GUID "46c93e13-ee9b-4262-8488-563bca757fef"

/* Supported WMI keys ... */
#define ACPI_CUT_PAYLOAD		0x00000201
#define ACPI_ALL_APPS_PAYLOAD		0x00000301
#define ACPI_SETUP_PAYLOAD		0x00001b01
#define ACPI_CST_KEY_PRESS_PAYLOAD	0x00011801
#define ACPI_CST_KEY_RELEASE_PAYLOAD	0x00011901

/* ... and their mappings */
#define WMI_CUT_KEY		KEY_PROG1
#define WMI_ALL_APPS_KEY	KEY_ALL_APPLICATIONS
#define WMI_SETUP_KEY		KEY_SETUP
#define WMI_CST_KEY		KEY_ASSISTANT

/* Keyboard backlight key (not supported yet) */
#define BACKLIGHT_LEVEL_0_PAYLOAD	0x00000501
#define BACKLIGHT_LEVEL_1_PAYLOAD	0x00800501
#define BACKLIGHT_LEVEL_2_PAYLOAD	0x00050501
#define BACKLIGHT_LEVEL_3_PAYLOAD	0x000a0501

struct redmi_wmi {
	struct input_dev *input_dev;
	/* Protects the key event sequence */
	struct mutex key_lock;
};

static void redmi_mutex_destroy(void *data)
{
	struct mutex *lock = data;

	mutex_destroy(lock);
}

static int redmi_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct redmi_wmi *data;
	int ret;

	/* Init dev */
	data = devm_kzalloc(&wdev->dev, sizeof(struct redmi_wmi), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_set_drvdata(&wdev->dev, data);

	/* Init mutex & setup destroy at exit */
	mutex_init(&data->key_lock);
	ret = devm_add_action_or_reset(&wdev->dev, redmi_mutex_destroy, &data->key_lock);
	if (ret < 0)
		return ret;

	/* Setup input device */
	data->input_dev = devm_input_allocate_device(&wdev->dev);
	if (!data->input_dev)
		return -ENOMEM;
	data->input_dev->name = "Redmibook WMI keys";
	data->input_dev->phys = "wmi/input0";

	set_bit(EV_KEY, data->input_dev->evbit);

	/* "Cut" key*/
	set_bit(WMI_CUT_KEY, data->input_dev->keybit);
	/* "All apps" key*/
	set_bit(WMI_ALL_APPS_KEY, data->input_dev->keybit);
	/* "Settings" key */
	set_bit(WMI_SETUP_KEY, data->input_dev->keybit);
	/* Custom (AI?) key */
	set_bit(WMI_CST_KEY, data->input_dev->keybit);

	return input_register_device(data->input_dev);
}

static void press_and_release_key(struct input_dev *dev, unsigned int code)
{
	input_report_key(dev, code, 1);
	input_sync(dev);
	input_report_key(dev, code, 0);
	input_sync(dev);
}

static void redmi_wmi_notify(struct wmi_device *wdev, union acpi_object *obj)
{
	struct redmi_wmi *data = dev_get_drvdata(&wdev->dev);

	if (obj->type != ACPI_TYPE_BUFFER) {
		dev_err(&wdev->dev, "Bad response type %u\n", obj->type);
		return;
	}

	if (obj->buffer.length < 4) {
		dev_err(&wdev->dev, "Invalid buffer length %u\n", obj->buffer.length);
		return;
	}

	/* For linearizability */
	guard(mutex)(&data->key_lock);

	u32 payload = ((u32 *)obj->buffer.pointer)[0];

	switch (payload) {
	case ACPI_CUT_PAYLOAD:
		press_and_release_key(data->input_dev, WMI_CUT_KEY);
		break;
	case ACPI_ALL_APPS_PAYLOAD:
		press_and_release_key(data->input_dev, WMI_ALL_APPS_KEY);
		break;
	case ACPI_SETUP_PAYLOAD:
		press_and_release_key(data->input_dev, WMI_SETUP_KEY);
		break;
	case ACPI_CST_KEY_PRESS_PAYLOAD:
		input_report_key(data->input_dev, WMI_CST_KEY, 1);
		input_sync(data->input_dev);
		break;
	case ACPI_CST_KEY_RELEASE_PAYLOAD:
		input_report_key(data->input_dev, WMI_CST_KEY, 0);
		input_sync(data->input_dev);
		break;
	case BACKLIGHT_LEVEL_0_PAYLOAD:
	case BACKLIGHT_LEVEL_1_PAYLOAD:
	case BACKLIGHT_LEVEL_2_PAYLOAD:
	case BACKLIGHT_LEVEL_3_PAYLOAD:
		pr_debug("keyboard backlight WMI event, no action");
		break;
	default:
		pr_debug("unsupported Redmibook WMI event with 4byte payload %u", payload);
		break;
	}
}

static const struct wmi_device_id redmi_wmi_id_table[] = {
	{ .guid_string = WMI_REDMIBOOK_KEYBOARD_EVENT_GUID },
	/* Terminating entry */
	{ }
};

static struct wmi_driver redmi_wmi_driver = {
	.driver = {
		.name = "redmi-wmi",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS
	},
	.id_table = redmi_wmi_id_table,
	.probe = redmi_wmi_probe,
	.notify = redmi_wmi_notify,
	.no_singleton = true,
};
module_wmi_driver(redmi_wmi_driver);

MODULE_DEVICE_TABLE(wmi, redmi_wmi_id_table);
MODULE_AUTHOR("Gladyshev Ilya <foxido@foxido.dev>");
MODULE_DESCRIPTION("Redmibook WMI driver");
MODULE_LICENSE("GPL");
