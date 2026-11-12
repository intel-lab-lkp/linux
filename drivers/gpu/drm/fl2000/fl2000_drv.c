// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"
#include "fl2000_it66121.h"

#define USB_VENDOR_FRESCO_LOGIC 0x1D5C
#define USB_PRODUCT_FL2000	0x2000

static struct usb_driver fl2000_driver;

/*
 * The three interfaces of the display function only work together, and the driver binds to the
 * device through the first one: it matches interface 0 and claims the others from its probe, as
 * other multi-interface USB drivers do. Everything else hangs off the binding of interface 0 and
 * goes away with it, whatever the reason.
 */
static const struct usb_device_id fl2000_id_table[] = {
	{
		.match_flags = USB_DEVICE_ID_MATCH_DEVICE | USB_DEVICE_ID_MATCH_INT_CLASS |
			       USB_DEVICE_ID_MATCH_INT_NUMBER,
		.idVendor = USB_VENDOR_FRESCO_LOGIC,
		.idProduct = USB_PRODUCT_FL2000,
		.bInterfaceClass = USB_CLASS_AUDIO_VIDEO,
		.bInterfaceNumber = FL2000_USBIF_AVCONTROL,
	},
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

struct fl2000_devs {
	struct regmap *regmap;
	struct i2c_adapter *adapter;
	struct drm_bridge *bridge;
	struct fl2000_device *fdev;
	/* The display was stopped for system sleep or a reset and has not been restarted yet */
	bool suspended;
};

/* Interface 0 is what the driver is probing: claim the others */
static int fl2000_claim_interfaces(struct usb_device *usb_dev, struct fl2000_devs *devs)
{
	int ret;

	for (int i = FL2000_USBIF_AVCONTROL + 1; i < FL2000_USBIF_NUM; i++) {
		ret = usb_driver_claim_interface(&fl2000_driver, usb_ifnum_to_if(usb_dev, i), devs);
		if (ret) {
			dev_err(&usb_dev->dev, "Cannot claim interface %d (%d)\n", i, ret);
			while (--i > FL2000_USBIF_AVCONTROL) {
				struct usb_interface *other = usb_ifnum_to_if(usb_dev, i);

				usb_set_intfdata(other, NULL);
				usb_driver_release_interface(&fl2000_driver, other);
			}
			return ret;
		}
	}

	return 0;
}

/* Let go of all interfaces but @interface, which is being unbound if it is one of them */
static void fl2000_release_interfaces(struct usb_device *usb_dev, struct usb_interface *interface)
{
	for (int i = 0; i < FL2000_USBIF_NUM; i++) {
		struct usb_interface *other = usb_ifnum_to_if(usb_dev, i);

		if (other != interface)
			usb_driver_release_interface(&fl2000_driver, other);
	}
}

static int fl2000_probe(struct usb_interface *interface, const struct usb_device_id *usb_dev_id)
{
	int ret;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs;

	if (usb_dev->speed < USB_SPEED_HIGH) {
		dev_err(&interface->dev, "USB 1.1 is not supported\n");
		return -ENODEV;
	}

	/* Frames go to the bulk endpoint of altsetting 1, and the chip has always been used so */
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(&interface->dev, "Cannot set streaming interface for bulk transfers\n");
		return ret;
	}

	devs = devm_kzalloc(&interface->dev, sizeof(*devs), GFP_KERNEL);
	if (!devs)
		return -ENOMEM;

	devs->regmap = fl2000_regmap_init(&interface->dev, usb_dev);
	if (IS_ERR(devs->regmap)) {
		dev_err(&interface->dev, "Cannot initialize regmap (%pe)\n", devs->regmap);
		return PTR_ERR(devs->regmap);
	}

	devs->adapter = fl2000_i2c_init(interface, devs->regmap);
	if (IS_ERR(devs->adapter)) {
		dev_err(&interface->dev, "Cannot initialize I2C adapter (%pe)\n", devs->adapter);
		return PTR_ERR(devs->adapter);
	}

	/* The IT66121 is looked for with the I2C engine, ready as for reading the EDID */
	fl2000_reset(devs->regmap);
	fl2000_usb_magic(devs->regmap);

	devs->bridge = it66121_create(devs->adapter);
	if (IS_ERR(devs->bridge))
		return PTR_ERR(devs->bridge);

	ret = fl2000_claim_interfaces(usb_dev, devs);
	if (ret)
		goto err_destroy_bridge;

	devs->fdev = fl2000_drm_create(interface, devs->regmap, devs->adapter, devs->bridge);
	if (IS_ERR(devs->fdev)) {
		ret = PTR_ERR(devs->fdev);
		goto err_release_interfaces;
	}

	usb_set_intfdata(interface, devs);

	return 0;

err_release_interfaces:
	for (int i = FL2000_USBIF_AVCONTROL + 1; i < FL2000_USBIF_NUM; i++)
		usb_set_intfdata(usb_ifnum_to_if(usb_dev, i), NULL);
	fl2000_release_interfaces(usb_dev, interface);
err_destroy_bridge:
	it66121_destroy(devs->bridge);

	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs = usb_get_intfdata(interface);

	/* This interface was only let go of, by the disconnect of another one */
	if (!devs)
		return;

	for (int i = 0; i < FL2000_USBIF_NUM; i++)
		usb_set_intfdata(usb_ifnum_to_if(usb_dev, i), NULL);

	fl2000_drm_destroy(devs->fdev);
	it66121_destroy(devs->bridge);

	/* Last, as devs goes with interface 0, which may be one of these */
	fl2000_release_interfaces(usb_dev, interface);
}

/*
 * System sleep. The USB core suspends the interfaces one by one, the last first, and resumes them
 * in order once the device is awake. The display is one thing for all three: stop it at the first
 * suspend, before any interface sleeps, and start it again at the first resume. The same callback
 * serves a reset-resume, after which the chips are set up from scratch: without one the USB core
 * would unbind and bind the driver again, which a running compositor sees as an unplug.
 */
static int fl2000_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct fl2000_devs *devs = usb_get_intfdata(interface);

	if (!devs || devs->suspended)
		return 0;

	/* Even if this fails: the USB core ignores the error during system sleep and goes on */
	devs->suspended = true;

	return fl2000_drm_suspend(devs->fdev);
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct fl2000_devs *devs = usb_get_intfdata(interface);

	if (!devs || !devs->suspended)
		return 0;

	devs->suspended = false;

	return fl2000_drm_resume(devs->fdev);
}

/*
 * A reset of the USB device (the storage function of the adapter asks for one when it times out)
 * is the same as far as the display goes. Nonzero would make the USB core unbind the driver.
 */
static int fl2000_pre_reset(struct usb_interface *interface)
{
	fl2000_suspend(interface, PMSG_SUSPEND);

	return 0;
}

static struct usb_driver fl2000_driver = {
	.name = KBUILD_MODNAME,
	.probe = fl2000_probe,
	.disconnect = fl2000_disconnect,
	.suspend = fl2000_suspend,
	.resume = fl2000_resume,
	.reset_resume = fl2000_resume,
	.pre_reset = fl2000_pre_reset,
	.post_reset = fl2000_resume,
	.id_table = fl2000_id_table,
	.supports_autosuspend = false,
	.disable_hub_initiated_lpm = true,
};

module_usb_driver(fl2000_driver);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL");
