// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"
#include "fl2000_it66121.h"

#define USB_CLASS_AV		0x10
#define USB_SUBCLASS_AV_CONTROL 0x01
#define USB_SUBCLASS_AV_VIDEO	0x02
#define USB_SUBCLASS_AV_AUDIO	0x03

#define USB_VENDOR_FRESCO_LOGIC 0x1D5C
#define USB_PRODUCT_FL2000	0x2000

#define FL2000_ALL_IFS \
	(BIT(FL2000_USBIF_AVCONTROL) | BIT(FL2000_USBIF_STREAMING) | BIT(FL2000_USBIF_INTERRUPT))

static const struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(USB_VENDOR_FRESCO_LOGIC, USB_PRODUCT_FL2000, USB_CLASS_AV) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

/* Devices that are independent of interfaces, created for the lifetime of the USB device */
struct fl2000_devs {
	struct regmap *regmap;
	struct i2c_adapter *adapter;
	struct i2c_client *bridge_client;
	struct component_match *match;
	int active_if;
	/* The display was stopped for system sleep and has not been restarted yet */
	bool suspended;
};

static const struct component_master_ops fl2000_master_ops = {
	.bind = fl2000_drm_bind,
	.unbind = fl2000_drm_unbind,
};

static int fl2000_compare(struct device *dev, void *data)
{
	struct i2c_client *client = i2c_verify_client(dev);
	struct i2c_adapter *adapter = data;
	static const char *const fl2000_supported_bridges[] = {
		"fl2000-it66121",
	};

	/*
	 * Only match clients sitting on this FL2000 instance's own I2C bus: a
	 * client with a matching name may belong to another FL2000 device that
	 * is disconnecting or re-enumerating concurrently.
	 */
	if (!client || client->adapter != adapter)
		return 0;

	/* Check this is a supported DRM bridge */
	for (int i = 0; i < ARRAY_SIZE(fl2000_supported_bridges); i++)
		if (!strncmp(fl2000_supported_bridges[i], client->name, sizeof(client->name)))
			return 1;

	return 0;
}

static struct fl2000_devs *fl2000_get_devices(struct usb_device *usb_dev)
{
	struct fl2000_devs *devs;

	devs = devm_kzalloc(&usb_dev->dev, sizeof(*devs), GFP_KERNEL);
	if (!devs)
		return ERR_PTR(-ENOMEM);

	devs->regmap = fl2000_regmap_init(usb_dev);
	if (IS_ERR(devs->regmap)) {
		dev_err(&usb_dev->dev, "Cannot initialize regmap (%pe)\n", devs->regmap);
		return ERR_CAST(devs->regmap);
	}

	devs->adapter = fl2000_i2c_init(usb_dev);
	if (IS_ERR(devs->adapter)) {
		dev_err(&usb_dev->dev, "Cannot initialize I2C adapter (%pe)\n", devs->adapter);
		return ERR_CAST(devs->adapter);
	}

	component_match_add(&devs->adapter->dev, &devs->match, fl2000_compare, devs->adapter);

	dev_set_drvdata(&usb_dev->dev, devs);

	return devs;
}

static void fl2000_bridge_release(struct device *dev, void *res)
{
	struct i2c_client **client = res;

	dev_dbg(dev, "Destroying IT66121 bridge\n");
	it66121_destroy(*client);
}

/*
 * Create the IT66121 bridge and tie its destruction to the usb_device
 * lifetime: it must survive component master del/add cycles (USB resets) and
 * die only when the USB device is actually gone. The devres is released
 * before the I2C adapter devres (LIFO), so the client is destroyed while its
 * adapter still exists.
 */
static int fl2000_bridge_create(struct usb_device *usb_dev, struct fl2000_devs *devs)
{
	struct i2c_client *client;
	struct i2c_client **holder;

	holder = devres_alloc(fl2000_bridge_release, sizeof(*holder), GFP_KERNEL);
	if (!holder)
		return -ENOMEM;

	client = it66121_create(devs->adapter);
	if (IS_ERR(client)) {
		dev_err(&usb_dev->dev, "Cannot create HDMI bridge (%pe)\n", client);
		devres_free(holder);
		return -ENODEV;
	}

	*holder = client;
	devres_add(&usb_dev->dev, holder);
	devs->bridge_client = client;

	return 0;
}

static int fl2000_probe(struct usb_interface *interface, const struct usb_device_id *usb_dev_id)
{
	int ret = 0;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs = dev_get_drvdata(&usb_dev->dev);

	if (usb_dev->speed < USB_SPEED_HIGH) {
		dev_err(&usb_dev->dev, "USB 1.1 is not supported\n");
		return -ENODEV;
	}

	if (!devs) {
		devs = fl2000_get_devices(usb_dev);
		if (IS_ERR(devs))
			return PTR_ERR(devs);
	}

	/*
	 * Create the IT66121 bridge if it does not exist yet. Retrying from
	 * every interface probe covers transient detection failures on cold
	 * boot. Once created, the bridge component persists for the lifetime of
	 * the usb_device (across usb_reset_device() unbind/rebind cycles):
	 * destroying and re-creating it per bind cycle leaves dangling
	 * component pointers in the aggregate match data and crashes on rebind.
	 */
	if (!devs->bridge_client) {
		ret = fl2000_bridge_create(usb_dev, devs);
		if (ret)
			return ret;
	}

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
	case FL2000_USBIF_STREAMING:
	case FL2000_USBIF_INTERRUPT:
		devs->active_if |= BIT(iface_num);
		break;

	default:
		/* Device does not have any other interfaces */
		dev_warn(&interface->dev, "Unexpected interface %d\n", iface_num);
		ret = -ENODEV;
		break;
	}

	/* When all interfaces are up - proceed with registration */
	if (devs->active_if == FL2000_ALL_IFS) {
		ret = component_master_add_with_match(&devs->adapter->dev, &fl2000_master_ops,
						      devs->match);
		if (ret) {
			dev_err(&usb_dev->dev, "Cannot register component master (%d)\n", ret);
			return ret;
		}
	}

	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs = dev_get_drvdata(&usb_dev->dev);

	if (!devs)
		return;

	/*
	 * The IT66121 bridge component is intentionally NOT destroyed here.
	 * Disconnect also runs for the transient unbind/rebind cycles of
	 * usb_reset_device(), where the usb_device (and the I2C adapter and
	 * aggregate match data) survive: the component must persist across
	 * those. Its destruction is a devres of the usb_device, see
	 * fl2000_bridge_create(). Destroying it from the master unbind path is
	 * also forbidden - component_del() would self-deadlock on the global
	 * component mutex held across the unbind callback.
	 */
	if (devs->active_if == FL2000_ALL_IFS)
		component_master_del(&devs->adapter->dev, &fl2000_master_ops);

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
	case FL2000_USBIF_STREAMING:
	case FL2000_USBIF_INTERRUPT:
		devs->active_if &= ~BIT(iface_num);
		break;

	default:
		/* Device does not have any other interfaces */
		dev_warn(&interface->dev, "Unexpected interface %d\n", iface_num);
		break;
	}
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
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs = dev_get_drvdata(&usb_dev->dev);
	int ret;

	if (!devs || devs->suspended)
		return 0;

	ret = fl2000_drm_suspend(&devs->adapter->dev);
	if (ret)
		return ret;

	devs->suspended = true;

	return 0;
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs = dev_get_drvdata(&usb_dev->dev);

	if (!devs || !devs->suspended)
		return 0;

	devs->suspended = false;

	return fl2000_drm_resume(&devs->adapter->dev);
}

static struct usb_driver fl2000_driver = {
	.name = KBUILD_MODNAME,
	.probe = fl2000_probe,
	.disconnect = fl2000_disconnect,
	.suspend = fl2000_suspend,
	.resume = fl2000_resume,
	.reset_resume = fl2000_resume,
	.id_table = fl2000_id_table,
	.supports_autosuspend = false,
	.disable_hub_initiated_lpm = true,
};

module_usb_driver(fl2000_driver);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL");
