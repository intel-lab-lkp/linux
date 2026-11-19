// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lenovo WMI hotkeys driver
 *
 * Lenovo consumer laptops deliver their vendor specific Fn keys (microphone
 * mute, airplane mode, performance mode, the star key and others) as WMI
 * events on GUID 8FC0DE0C-B4E4-43FD-B0F3-8871711C1294 instead of keyboard
 * usages, and the older Yoga models report Fn+Esc on two dedicated event
 * GUIDs that carry no data. This driver owns these event GUIDs on every
 * architecture and reports the keys through a sparse keymap. Drivers with
 * platform knowledge, such as ideapad-laptop, subscribe to the notifier chain
 * to act on an event and may stop the key from being reported.
 *
 * The key codes follow the mapping historically used by ideapad-laptop.
 */

#include <linux/device.h>
#include <linux/export.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_data/x86/lenovo-wmi-hotkeys.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include <asm/byteorder.h>

#define LENOVO_FN_KEYS_WMI_GUID	"8FC0DE0C-B4E4-43FD-B0F3-8871711C1294"
/* Fn+Esc on the Yoga 3 and Yoga 700 generations, no event data */
#define LENOVO_ESC_WMI_GUID_YOGA3	"26CAB2E5-5CF1-46AE-AAC3-4A12B6BA50E6"
#define LENOVO_ESC_WMI_GUID_YOGA700	"56322276-8493-4CE8-A783-98C991274F5E"

/*
 * The Esc event GUIDs carry no key code; they are reported under
 * LENOVO_WMI_HOTKEYS_ESC_CODE, the code ideapad-laptop has always used for
 * them, so the existing hwdb mappings keep applying. The Fn key GUID codes
 * are all below 0x80.
 */

enum lenovo_wmi_hotkeys_type {
	LENOVO_WMI_HOTKEYS_FN_KEYS,
	LENOVO_WMI_HOTKEYS_ESC,
};

static const enum lenovo_wmi_hotkeys_type lenovo_wmi_hotkeys_fn_keys = LENOVO_WMI_HOTKEYS_FN_KEYS;
static const enum lenovo_wmi_hotkeys_type lenovo_wmi_hotkeys_esc = LENOVO_WMI_HOTKEYS_ESC;

static BLOCKING_NOTIFIER_HEAD(lenovo_wmi_hotkeys_chain);

static const struct key_entry lenovo_wmi_hotkeys_keymap[] = {
	/* Fn+Esc from the Esc event GUIDs */
	{ KE_KEY, LENOVO_WMI_HOTKEYS_ESC_CODE, { KEY_ESC } },
	/* Customizable Lenovo Hotkey ("star" with 'S' inside) */
	{ KE_KEY, 0x01, { KEY_FAVORITES } },
	/* Fn lock toggled on / off, handled by the firmware */
	{ KE_IGNORE, 0x02 },
	/* Esc, handled by the firmware */
	{ KE_IGNORE, 0x03 },
	{ KE_KEY, 0x04, { KEY_SELECTIVE_SCREENSHOT } },
	/* Lenovo Support */
	{ KE_KEY, 0x07, { KEY_HELP } },
	/* Refresh Rate Toggle */
	{ KE_KEY, 0x0a, { KEY_REFRESH_RATE_TOGGLE } },
	/* Camera lens cover closed / opened */
	{ KE_SW, 0x0c, .sw = { SW_CAMERA_LENS_COVER, 0 } },
	{ KE_SW, 0x0d, .sw = { SW_CAMERA_LENS_COVER, 1 } },
	{ KE_KEY, 0x0e, { KEY_PICKUP_PHONE } },
	{ KE_KEY, 0x0f, { KEY_HANGUP_PHONE } },
	/* Refresh Rate Toggle (Fn+R) */
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
	/* Performance mode toggle, Fn+Q */
	{ KE_KEY, 0x3d, { KEY_PERFORMANCE } },
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
	/* Mode, opens a profile chooser; the Fn+Q button cycles the profile */
	{ KE_KEY, 0x46, { KEY_PROG4 } },
	/* Lenovo Smart Connect */
	{ KE_KEY, 0x48, { KEY_LINK_PHONE } },
	{ KE_END },
};

struct lenovo_wmi_hotkeys {
	struct input_dev *input;
	enum lenovo_wmi_hotkeys_type type;
};

/**
 * lenovo_wmi_hotkeys_register_notifier() - Subscribe to the hotkey events.
 * @nb: The notifier_block to register.
 *
 * The callback runs in process context with the WMI event code as the
 * action. Return NOTIFY_STOP from the callback to consume the event.
 *
 * Return: 0 on success, or a negative error code.
 */
int lenovo_wmi_hotkeys_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&lenovo_wmi_hotkeys_chain, nb);
}
EXPORT_SYMBOL_NS_GPL(lenovo_wmi_hotkeys_register_notifier, "LENOVO_WMI_HOTKEYS");

/**
 * lenovo_wmi_hotkeys_unregister_notifier() - Unsubscribe from the hotkey events.
 * @nb: The notifier_block to unregister.
 *
 * Return: 0 on success, or a negative error code.
 */
int lenovo_wmi_hotkeys_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&lenovo_wmi_hotkeys_chain, nb);
}
EXPORT_SYMBOL_NS_GPL(lenovo_wmi_hotkeys_unregister_notifier, "LENOVO_WMI_HOTKEYS");

static void devm_lenovo_wmi_hotkeys_unregister_notifier(void *data)
{
	lenovo_wmi_hotkeys_unregister_notifier(data);
}

/**
 * devm_lenovo_wmi_hotkeys_register_notifier() - Device managed subscription.
 * @dev: The device whose lifetime bounds the subscription.
 * @nb: The notifier_block to register.
 *
 * Return: 0 on success, or a negative error code.
 */
int devm_lenovo_wmi_hotkeys_register_notifier(struct device *dev,
					      struct notifier_block *nb)
{
	int ret;

	ret = lenovo_wmi_hotkeys_register_notifier(nb);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, devm_lenovo_wmi_hotkeys_unregister_notifier, nb);
}
EXPORT_SYMBOL_NS_GPL(devm_lenovo_wmi_hotkeys_register_notifier, "LENOVO_WMI_HOTKEYS");

static int lenovo_wmi_hotkeys_probe(struct wmi_device *wdev, const void *context)
{
	struct lenovo_wmi_hotkeys *priv;
	int ret;

	priv = devm_kzalloc_obj(&wdev->dev, *priv);
	if (!priv)
		return -ENOMEM;

	priv->type = *(const enum lenovo_wmi_hotkeys_type *)context;

	priv->input = devm_input_allocate_device(&wdev->dev);
	if (!priv->input)
		return -ENOMEM;

	/*
	 * Same name as the ideapad-laptop input device so that the existing
	 * hwdb keyboard mappings for these keys keep applying.
	 */
	priv->input->name = "Ideapad extra buttons";
	priv->input->phys = priv->type == LENOVO_WMI_HOTKEYS_ESC ? "wmi/input1" : "wmi/input0";
	priv->input->id.bustype = BUS_HOST;
	priv->input->dev.parent = &wdev->dev;

	ret = sparse_keymap_setup(priv->input, lenovo_wmi_hotkeys_keymap, NULL);
	if (ret)
		return ret;

	dev_set_drvdata(&wdev->dev, priv);

	return input_register_device(priv->input);
}

static void lenovo_wmi_hotkeys_notify(struct wmi_device *wdev, const struct wmi_buffer *buf)
{
	struct lenovo_wmi_hotkeys *priv = dev_get_drvdata(&wdev->dev);
	u32 code;
	int ret;

	if (priv->type == LENOVO_WMI_HOTKEYS_ESC) {
		code = LENOVO_WMI_HOTKEYS_ESC_CODE;
	} else {
		if (buf->length < sizeof(u32)) {
			dev_dbg(&wdev->dev, "short hotkey event (%zu bytes)\n", buf->length);
			return;
		}
		code = le32_to_cpup(buf->data);
	}

	dev_dbg(&wdev->dev, "hotkey event 0x%x\n", code);

	ret = blocking_notifier_call_chain(&lenovo_wmi_hotkeys_chain, code, NULL);
	if (ret & NOTIFY_STOP_MASK)
		return;

	if (!sparse_keymap_report_event(priv->input, code, 1, true))
		dev_info_ratelimited(&wdev->dev, "unknown hotkey event 0x%x\n", code);
}

static const struct wmi_device_id lenovo_wmi_hotkeys_id_table[] = {
	{ LENOVO_FN_KEYS_WMI_GUID, &lenovo_wmi_hotkeys_fn_keys },
	{ LENOVO_ESC_WMI_GUID_YOGA3, &lenovo_wmi_hotkeys_esc },
	{ LENOVO_ESC_WMI_GUID_YOGA700, &lenovo_wmi_hotkeys_esc },
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
	.notify_new = lenovo_wmi_hotkeys_notify,
	.no_singleton = true,
};
module_wmi_driver(lenovo_wmi_hotkeys_driver);

MODULE_DESCRIPTION("Lenovo WMI hotkeys driver");
MODULE_LICENSE("GPL");
