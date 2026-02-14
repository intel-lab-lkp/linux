// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/array_size.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/mfd/asus-transformer-ec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define ASUSEC_EXT_KEY_CODES		0x20

struct asus_ec_keys_data {
	struct notifier_block nb;
	struct asusec_info *ec;
	struct input_dev *xidev;
	bool special_key_pressed;
	bool special_key_mode;
	unsigned short keymap[ASUSEC_EXT_KEY_CODES * 2];
};

static void asus_ec_input_event(struct input_handle *handle,
				unsigned int event_type,
				unsigned int event_code, int value)
{
	struct asus_ec_keys_data *priv = handle->handler->private;

	/* Store special key state */
	if (event_type == EV_KEY && event_code == KEY_RIGHTALT)
		priv->special_key_pressed = !!value;
}

static int asus_ec_input_connect(struct input_handler *handler, struct input_dev *dev,
				 const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "asusec-media-handler";

	error = input_register_handle(handle);
	if (error)
		goto err_free_handle;

	error = input_open_device(handle);
	if (error)
		goto err_unregister_handle;

	return 0;

 err_unregister_handle:
	input_unregister_handle(handle);
 err_free_handle:
	kfree(handle);

	return error;
}

static void asus_ec_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id asus_ec_input_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ }
};

static struct input_handler asus_ec_input_handler = {
	.name =	"asusec-media-handler",
	.event = asus_ec_input_event,
	.connect = asus_ec_input_connect,
	.disconnect = asus_ec_input_disconnect,
	.id_table = asus_ec_input_ids,
};

static const unsigned short asus_ec_dock_ext_keys[] = {
	/* Function keys [0x00 - 0x19] */
	[0x01] = KEY_DELETE,
	[0x02] = KEY_F1,
	[0x03] = KEY_F2,
	[0x04] = KEY_F3,
	[0x05] = KEY_F4,
	[0x06] = KEY_F5,
	[0x07] = KEY_F6,
	[0x08] = KEY_F7,
	[0x10] = KEY_F8,
	[0x11] = KEY_F9,
	[0x12] = KEY_F10,
	[0x13] = KEY_F11,
	[0x14] = KEY_F12,
	[0x15] = KEY_MUTE,
	[0x16] = KEY_VOLUMEDOWN,
	[0x17] = KEY_VOLUMEUP,
	/* Multimedia keys [0x20 - 0x39] */
	[0x21] = KEY_SCREENLOCK,
	[0x22] = KEY_WLAN,
	[0x23] = KEY_BLUETOOTH,
	[0x24] = KEY_TOUCHPAD_TOGGLE,
	[0x25] = KEY_BRIGHTNESSDOWN,
	[0x26] = KEY_BRIGHTNESSUP,
	[0x27] = KEY_BRIGHTNESS_AUTO,
	[0x28] = KEY_PRINT,
	[0x30] = KEY_WWW,
	[0x31] = KEY_CONFIG,
	[0x32] = KEY_PREVIOUSSONG,
	[0x33] = KEY_PLAYPAUSE,
	[0x34] = KEY_NEXTSONG,
	[0x35] = KEY_MUTE,
	[0x36] = KEY_VOLUMEDOWN,
	[0x37] = KEY_VOLUMEUP,
};

static void asus_ec_keys_report_key(struct input_dev *dev, unsigned int code,
				    unsigned int key, bool value)
{
	input_event(dev, EV_MSC, MSC_SCAN, code);
	input_report_key(dev, key, value);
	input_sync(dev);
}

static int asus_ec_keys_process_key(struct input_dev *dev, u8 code)
{
	struct asus_ec_keys_data *priv = dev_get_drvdata(dev->dev.parent);
	unsigned int key = 0;

	if (code == 0)
		return NOTIFY_DONE;

	/* Flip special key mode state when pressing key 1 with special key pressed */
	if (priv->special_key_pressed && code == 1) {
		priv->special_key_mode = !priv->special_key_mode;
		return NOTIFY_DONE;
	}

	/*
	 * Relocate code to second "page" if pressed state XOR's mode state
	 * This way special key will invert the current mode
	 */
	if (priv->special_key_mode ^ priv->special_key_pressed)
		code += ASUSEC_EXT_KEY_CODES;

	if (code < dev->keycodemax) {
		unsigned short *map = dev->keycode;

		key = map[code];
	}

	if (!key)
		key = KEY_UNKNOWN;

	asus_ec_keys_report_key(dev, code, key, 1);
	asus_ec_keys_report_key(dev, code, key, 0);

	return NOTIFY_OK;
}

static int asus_ec_keys_notify(struct notifier_block *nb,
			       unsigned long action, void *data_)
{
	struct asus_ec_keys_data *priv = container_of(nb, struct asus_ec_keys_data, nb);
	u8 *data = data_;

	if (action & ASUSEC_SMI_MASK)
		return NOTIFY_DONE;

	if (action & ASUSEC_SCI_MASK)
		return asus_ec_keys_process_key(priv->xidev, data[2]);

	return NOTIFY_DONE;
}

static void asus_ec_keys_setup_keymap(struct asus_ec_keys_data *priv)
{
	struct input_dev *dev = priv->xidev;
	unsigned int i;

	BUILD_BUG_ON(ARRAY_SIZE(priv->keymap) < ARRAY_SIZE(asus_ec_dock_ext_keys));

	dev->keycode = priv->keymap;
	dev->keycodesize = sizeof(*priv->keymap);
	dev->keycodemax = ARRAY_SIZE(priv->keymap);

	input_set_capability(dev, EV_MSC, MSC_SCAN);
	input_set_capability(dev, EV_KEY, KEY_UNKNOWN);

	for (i = 0; i < ARRAY_SIZE(asus_ec_dock_ext_keys); i++) {
		unsigned int code = asus_ec_dock_ext_keys[i];

		if (!code)
			continue;

		__set_bit(code, dev->keybit);
		priv->keymap[i] = code;
	}
}

static void asus_ec_input_handler_deregister(void *priv)
{
	input_unregister_handler(&asus_ec_input_handler);
}

static int asus_ec_keys_probe(struct platform_device *pdev)
{
	struct asusec_info *ec = cell_to_ec(pdev);
	struct i2c_client *parent = to_i2c_client(pdev->dev.parent);
	struct asus_ec_keys_data *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->ec = ec;

	priv->xidev = devm_input_allocate_device(&pdev->dev);
	if (!priv->xidev)
		return -ENOMEM;

	priv->xidev->name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					   "%s Keyboard Ext", ec->model);
	priv->xidev->phys = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					   "i2c-%u-%04x",
					   i2c_adapter_id(parent->adapter),
					   parent->addr);
	asus_ec_keys_setup_keymap(priv);

	ret = input_register_device(priv->xidev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register extension keys: %d\n",
			ret);
		return ret;
	}

	asus_ec_input_handler.private = priv;

	ret = input_register_handler(&asus_ec_input_handler);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&pdev->dev, asus_ec_input_handler_deregister,
				       priv);
	if (ret)
		return ret;

	priv->nb.notifier_call = asus_ec_keys_notify;

	return devm_asus_ec_register_notifier(pdev, &priv->nb);
}

static struct platform_driver asus_ec_keys_driver = {
	.driver.name = "asus-transformer-ec-keys",
	.probe = asus_ec_keys_probe,
};
module_platform_driver(asus_ec_keys_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer's multimedia keys driver");
MODULE_LICENSE("GPL");
