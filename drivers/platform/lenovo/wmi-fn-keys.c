// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lenovo WMI Fn keys driver
 *
 * Lenovo consumer laptops deliver their vendor specific Fn keys (microphone
 * mute, airplane mode, performance mode, the star key and others) as WMI
 * events on a dedicated GUID instead of keyboard usages, and the older Yoga
 * models report Fn+Esc on two event GUIDs that carry no data. This driver
 * owns these event GUIDs on every architecture and reports the keys through
 * a sparse keymap. Drivers with platform knowledge, such as ideapad-laptop,
 * subscribe to the notifier chain to act on an event and may stop the key
 * from being reported.
 *
 * The keymap and the scan codes were moved here from ideapad-laptop, so the
 * input events are the same as before for userspace.
 */

#include <linux/device.h>
#include <linux/export.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_data/lenovo-wmi-fn-keys.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include <asm/byteorder.h>

#define LWMI_FN_KEYS_GUID	"8FC0DE0C-B4E4-43FD-B0F3-8871711C1294"
#define LWMI_ESC_GUID_YOGA3	"26CAB2E5-5CF1-46AE-AAC3-4A12B6BA50E6"
#define LWMI_ESC_GUID_YOGA700	"56322276-8493-4CE8-A783-98C991274F5E"

/*
 * Scan codes as ideapad-laptop reported them: the Fn key event codes are
 * offset by LWMI_FN_KEY so they do not collide with the ACPI hotkeys of that
 * driver, and the Fn+Esc events, which carry no data, use scan code 128.
 */
#define LWMI_FN_KEY		0x100
#define LWMI_ESC_SCANCODE	128

/**
 * struct lwmi_fn_keys_info - per event GUID description
 * @event: event type passed to the notifier chain
 * @phys: physical path of the input device
 * @event_size: size of the WMI event data, 0 when the GUID carries none
 * @code: scan code base: the offset added to the event data, or the whole
 *        scan code for GUIDs without event data
 * @keymap: sparse keymap for the input device
 */
struct lwmi_fn_keys_info {
	enum lwmi_fn_keys_event event;
	const char *phys;
	size_t event_size;
	u32 code;
	const struct key_entry *keymap;
};

struct lwmi_fn_keys {
	struct input_dev *input;
	const struct lwmi_fn_keys_info *info;
};

static BLOCKING_NOTIFIER_HEAD(lwmi_fn_keys_chain);

static const struct key_entry lwmi_fn_keys_keymap[] = {
	/* FnLock (handled by the firmware) */
	{ KE_IGNORE,	0x02 | LWMI_FN_KEY },
	/* Esc (handled by the firmware) */
	{ KE_IGNORE,	0x03 | LWMI_FN_KEY },

	/* Customizable Lenovo Hotkey ("star" with 'S' inside) */
	{ KE_KEY,	0x01 | LWMI_FN_KEY, { KEY_FAVORITES } },
	{ KE_KEY,	0x04 | LWMI_FN_KEY, { KEY_SELECTIVE_SCREENSHOT } },
	/* Lenovo Support */
	{ KE_KEY,	0x07 | LWMI_FN_KEY, { KEY_HELP } },
	{ KE_KEY,	0x0e | LWMI_FN_KEY, { KEY_PICKUP_PHONE } },
	{ KE_KEY,	0x0f | LWMI_FN_KEY, { KEY_HANGUP_PHONE } },
	/* Refresh Rate Toggle (Fn+R) */
	{ KE_KEY,	0x10 | LWMI_FN_KEY, { KEY_REFRESH_RATE_TOGGLE } },
	/* Dark mode toggle */
	{ KE_KEY,	0x13 | LWMI_FN_KEY, { KEY_PROG1 } },
	/* Sound profile switch */
	{ KE_KEY,	0x12 | LWMI_FN_KEY, { KEY_PROG2 } },
	/* Lenovo Virtual Background application */
	{ KE_KEY,	0x28 | LWMI_FN_KEY, { KEY_PROG3 } },
	/* Lenovo Support */
	{ KE_KEY,	0x27 | LWMI_FN_KEY, { KEY_HELP } },
	/* Refresh Rate Toggle */
	{ KE_KEY,	0x0a | LWMI_FN_KEY, { KEY_REFRESH_RATE_TOGGLE } },
	/* Specific to some newer models */
	{ KE_KEY,	0x3e | LWMI_FN_KEY, { KEY_MICMUTE } },
	{ KE_KEY,	0x3f | LWMI_FN_KEY, { KEY_RFKILL } },
	/* Star- (User Assignable Key) */
	{ KE_KEY,	0x44 | LWMI_FN_KEY, { KEY_PROG1 } },
	/* Eye */
	{ KE_KEY,	0x45 | LWMI_FN_KEY, { KEY_PROG3 } },
	/* Lenovo Smart Connect */
	{ KE_KEY,	0x48 | LWMI_FN_KEY, { KEY_LINK_PHONE } },
	/*
	 * Performance toggle also Fn+Q, consumed by ideapad-laptop through the
	 * notifier chain on machines where it cycles the platform profile
	 */
	{ KE_KEY,	0x3d | LWMI_FN_KEY, { KEY_PERFORMANCE } },
	/* Mode, opens a profile chooser; the Fn+Q button cycles the profile */
	{ KE_KEY,	0x46 | LWMI_FN_KEY, { KEY_PROG4 } },
	/* shift + prtsc */
	{ KE_KEY,	0x2d | LWMI_FN_KEY, { KEY_SELECTIVE_SCREENSHOT } },
	{ KE_KEY,	0x29 | LWMI_FN_KEY, { KEY_TOUCHPAD_TOGGLE } },
	{ KE_KEY,	0x2a | LWMI_FN_KEY, { KEY_ROOT_MENU } },
	/*
	 * Keyboard backlight level after Fn+Space, handled by the firmware:
	 * 0x40 off, 0x41 low, 0x42 high
	 */
	{ KE_IGNORE,	0x40 | LWMI_FN_KEY },
	{ KE_IGNORE,	0x41 | LWMI_FN_KEY },
	{ KE_IGNORE,	0x42 | LWMI_FN_KEY },
	{ KE_IGNORE,	0x43 | LWMI_FN_KEY },

	/*
	 * WMI switches
	 *
	 * Some devices come with physical switches. Whenever a switch is
	 * toggled, an WMI event is emitted to inform the software about the
	 * switch's latest state.
	 */

	/* Camera switch */
	{ KE_SW,	0x0c | LWMI_FN_KEY, .sw = { SW_CAMERA_LENS_COVER, 0 } },
	{ KE_SW,	0x0d | LWMI_FN_KEY, .sw = { SW_CAMERA_LENS_COVER, 1 } },

	{ KE_END }
};

static const struct key_entry lwmi_esc_keymap[] = {
	{ KE_KEY,	LWMI_ESC_SCANCODE, { KEY_ESC } },
	{ KE_END }
};

static const struct lwmi_fn_keys_info lwmi_fn_keys_info_fn_keys = {
	.event = LWMI_FN_KEYS_EVENT_FN_KEY,
	.phys = "wmi/input0",
	.event_size = sizeof(u32),
	.code = LWMI_FN_KEY,
	.keymap = lwmi_fn_keys_keymap,
};

static const struct lwmi_fn_keys_info lwmi_fn_keys_info_esc = {
	.event = LWMI_FN_KEYS_EVENT_ESC,
	.phys = "wmi/input1",
	.code = LWMI_ESC_SCANCODE,
	.keymap = lwmi_esc_keymap,
};

/**
 * lwmi_fn_keys_register_notifier() - Subscribe to the Fn key events.
 * @nb: The notifier_block to register.
 *
 * The callback runs in process context with the &enum lwmi_fn_keys_event as
 * the action and, for LWMI_FN_KEYS_EVENT_FN_KEY, a pointer to the u32 key
 * code as the data. Return NOTIFY_STOP from the callback to consume the event
 * so that no key is reported.
 *
 * Return: 0 on success, or a negative error code.
 */
int lwmi_fn_keys_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&lwmi_fn_keys_chain, nb);
}
EXPORT_SYMBOL_NS_GPL(lwmi_fn_keys_register_notifier, "LENOVO_WMI_FN_KEYS");

/**
 * lwmi_fn_keys_unregister_notifier() - Unsubscribe from the Fn key events.
 * @nb: The notifier_block to unregister.
 *
 * Return: 0 on success, or a negative error code.
 */
int lwmi_fn_keys_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&lwmi_fn_keys_chain, nb);
}
EXPORT_SYMBOL_NS_GPL(lwmi_fn_keys_unregister_notifier, "LENOVO_WMI_FN_KEYS");

static void devm_lwmi_fn_keys_unregister_notifier(void *data)
{
	lwmi_fn_keys_unregister_notifier(data);
}

/**
 * devm_lwmi_fn_keys_register_notifier() - Device managed subscription.
 * @dev: The device whose lifetime bounds the subscription.
 * @nb: The notifier_block to register.
 *
 * Return: 0 on success, or a negative error code.
 */
int devm_lwmi_fn_keys_register_notifier(struct device *dev, struct notifier_block *nb)
{
	int ret;

	ret = lwmi_fn_keys_register_notifier(nb);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, devm_lwmi_fn_keys_unregister_notifier, nb);
}
EXPORT_SYMBOL_NS_GPL(devm_lwmi_fn_keys_register_notifier, "LENOVO_WMI_FN_KEYS");

static int lwmi_fn_keys_probe(struct wmi_device *wdev, const void *context)
{
	struct lwmi_fn_keys *priv;
	int ret;

	priv = devm_kzalloc_obj(&wdev->dev, *priv);
	if (!priv)
		return -ENOMEM;

	priv->info = context;

	priv->input = devm_input_allocate_device(&wdev->dev);
	if (!priv->input)
		return -ENOMEM;

	/*
	 * Same name as the ideapad-laptop input device so that the existing
	 * hwdb keyboard mappings for these keys keep applying.
	 */
	priv->input->name = "Ideapad extra buttons";
	priv->input->phys = priv->info->phys;
	priv->input->id.bustype = BUS_HOST;
	priv->input->dev.parent = &wdev->dev;

	ret = sparse_keymap_setup(priv->input, priv->info->keymap, NULL);
	if (ret)
		return ret;

	dev_set_drvdata(&wdev->dev, priv);

	return input_register_device(priv->input);
}

static void lwmi_fn_keys_notify(struct wmi_device *wdev, const struct wmi_buffer *buf)
{
	struct lwmi_fn_keys *priv = dev_get_drvdata(&wdev->dev);
	const struct lwmi_fn_keys_info *info = priv->info;
	u32 code = 0;
	int ret;

	if (buf->length < info->event_size) {
		dev_dbg(&wdev->dev, "short event (%zu bytes)\n", buf->length);
		return;
	}

	if (info->event_size)
		code = le32_to_cpup(buf->data);

	dev_dbg(&wdev->dev, "event %d code 0x%x\n", info->event, code);

	ret = blocking_notifier_call_chain(&lwmi_fn_keys_chain, info->event, &code);
	if (ret & NOTIFY_STOP_MASK)
		return;

	if (!sparse_keymap_report_event(priv->input, info->code | code, 1, true))
		dev_info_ratelimited(&wdev->dev, "unknown key code 0x%x\n", code);
}

static const struct wmi_device_id lwmi_fn_keys_id_table[] = {
	{ LWMI_FN_KEYS_GUID, &lwmi_fn_keys_info_fn_keys },
	{ LWMI_ESC_GUID_YOGA3, &lwmi_fn_keys_info_esc },
	{ LWMI_ESC_GUID_YOGA700, &lwmi_fn_keys_info_esc },
	{ }
};
MODULE_DEVICE_TABLE(wmi, lwmi_fn_keys_id_table);

static struct wmi_driver lwmi_fn_keys_driver = {
	.driver = {
		.name = "lenovo-wmi-fn-keys",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = lwmi_fn_keys_id_table,
	.probe = lwmi_fn_keys_probe,
	.notify_new = lwmi_fn_keys_notify,
	.no_singleton = true,
};
module_wmi_driver(lwmi_fn_keys_driver);

MODULE_AUTHOR("David Cemin <dcemin@nvidia.com>");
MODULE_DESCRIPTION("Lenovo WMI Fn keys driver");
MODULE_LICENSE("GPL");
