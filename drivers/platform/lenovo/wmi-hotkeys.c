// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lenovo WMI hotkey driver
 *
 * Lenovo consumer laptops deliver their vendor specific Fn keys (microphone
 * mute, airplane mode, performance mode, the star key and others) as WMI
 * events on GUID 8FC0DE0C-B4E4-43FD-B0F3-8871711C1294 instead of keyboard
 * usages.  On x86 ideapad-laptop consumes these events; that driver depends
 * on i8042 and the IdeaPad VPC device, neither of which exist on the arm64
 * platforms.  This driver consumes the same event and reports the keys the
 * same way, so desktops see the usual generic key codes.
 *
 * The key codes follow the mapping used by ideapad-laptop.
 */

#include <linux/acpi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/module.h>
#include <linux/wmi.h>

#define LENOVO_FN_KEYS_WMI_GUID	"8FC0DE0C-B4E4-43FD-B0F3-8871711C1294"

static const struct key_entry lenovo_wmi_hotkeys_keymap[] = {
	/* Customizable Lenovo Hotkey ("star" with 'S' inside) */
	{ KE_KEY, 0x01, { KEY_FAVORITES } },
	/* Fn lock toggled on / off, firmware handles the LED */
	{ KE_IGNORE, 0x02 },
	{ KE_IGNORE, 0x03 },
	{ KE_KEY, 0x04, { KEY_SELECTIVE_SCREENSHOT } },
	/* Lenovo Support */
	{ KE_KEY, 0x07, { KEY_HELP } },
	{ KE_KEY, 0x0a, { KEY_REFRESH_RATE_TOGGLE } },
	{ KE_KEY, 0x0e, { KEY_PICKUP_PHONE } },
	{ KE_KEY, 0x0f, { KEY_HANGUP_PHONE } },
	{ KE_KEY, 0x10, { KEY_REFRESH_RATE_TOGGLE } },
	/* Sound profile switch */
	{ KE_KEY, 0x12, { KEY_PROG2 } },
	/* Dark mode toggle */
	{ KE_KEY, 0x13, { KEY_PROG1 } },
	/* Lenovo Support */
	{ KE_KEY, 0x27, { KEY_HELP } },
	/* Lenovo Virtual Background application */
	{ KE_KEY, 0x28, { KEY_PROG3 } },
	{ KE_KEY, 0x29, { KEY_TOUCHPAD_TOGGLE } },
	{ KE_KEY, 0x2a, { KEY_ROOT_MENU } },
	/* Shift + PrtSc */
	{ KE_KEY, 0x2d, { KEY_SELECTIVE_SCREENSHOT } },
	/* Performance mode, Fn+Q */
	{ KE_KEY, 0x3d, { KEY_PROG4 } },
	{ KE_KEY, 0x3e, { KEY_MICMUTE } },
	{ KE_KEY, 0x3f, { KEY_RFKILL } },
	/*
	 * Keyboard backlight level after Fn+Space, handled by the firmware:
	 * 0x40 off, 0x41 low, 0x42 high, 0x43 also seen
	 */
	{ KE_IGNORE, 0x40 },
	{ KE_IGNORE, 0x41 },
	{ KE_IGNORE, 0x42 },
	{ KE_IGNORE, 0x43 },
	/* Star, user assignable key */
	{ KE_KEY, 0x44, { KEY_PROG1 } },
	/* Eye care */
	{ KE_KEY, 0x45, { KEY_PROG3 } },
	/* Smart Connect, phone to PC link */
	{ KE_KEY, 0x48, { KEY_PROG2 } },
	{ KE_END },
};

struct lenovo_wmi_hotkeys {
	struct input_dev *input;
};

static int lenovo_wmi_hotkeys_probe(struct wmi_device *wdev, const void *context)
{
	struct lenovo_wmi_hotkeys *priv;
	int ret;

	/*
	 * ideapad-laptop binds the same GUID and handles these events together
	 * with the IdeaPad VPC device (Fn lock LED, DYTC performance mode
	 * cycling).  Leave those machines to it.
	 */
	if (acpi_dev_found("VPC2004")) {
		dev_dbg(&wdev->dev, "IdeaPad VPC present, deferring to ideapad-laptop\n");
		return -ENODEV;
	}

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->input = devm_input_allocate_device(&wdev->dev);
	if (!priv->input)
		return -ENOMEM;

	priv->input->name = "Lenovo WMI hotkeys";
	priv->input->phys = "wmi/input0";
	priv->input->id.bustype = BUS_HOST;
	priv->input->dev.parent = &wdev->dev;

	ret = sparse_keymap_setup(priv->input, lenovo_wmi_hotkeys_keymap, NULL);
	if (ret)
		return ret;

	ret = input_register_device(priv->input);
	if (ret)
		return ret;

	dev_set_drvdata(&wdev->dev, priv);

	return 0;
}

static void lenovo_wmi_hotkeys_notify(struct wmi_device *wdev, union acpi_object *data)
{
	struct lenovo_wmi_hotkeys *priv = dev_get_drvdata(&wdev->dev);
	u32 code;

	if (!data || data->type != ACPI_TYPE_INTEGER) {
		dev_dbg(&wdev->dev, "event data is not an integer\n");
		return;
	}

	code = data->integer.value;
	dev_dbg(&wdev->dev, "hotkey event 0x%x\n", code);

	if (!sparse_keymap_report_event(priv->input, code, 1, true))
		dev_info_ratelimited(&wdev->dev, "unknown hotkey event 0x%x\n", code);
}

static const struct wmi_device_id lenovo_wmi_hotkeys_id_table[] = {
	{ LENOVO_FN_KEYS_WMI_GUID, NULL },
	{ }
};
MODULE_DEVICE_TABLE(wmi, lenovo_wmi_hotkeys_id_table);

static struct wmi_driver lenovo_wmi_hotkeys_driver = {
	.driver = {
		.name = "lenovo-wmi-hotkeys",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = lenovo_wmi_hotkeys_id_table,
	.probe = lenovo_wmi_hotkeys_probe,
	.notify = lenovo_wmi_hotkeys_notify,
	.no_singleton = true,
};
module_wmi_driver(lenovo_wmi_hotkeys_driver);

MODULE_DESCRIPTION("Lenovo WMI hotkey driver");
MODULE_LICENSE("GPL");
