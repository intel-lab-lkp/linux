// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for setting auto suspend mode for Motorola Solutions security accessories
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>

#define DRIVER_DESC "Motorola Solutions security accessory driver"

static int motsol_sa_probe(struct usb_interface *interface,
			 const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);

	dev_dbg(&(interface)->dev, "probe (%04X:%04X)\n", id->idVendor,
		id->idProduct);
	usb_enable_autosuspend(udev);
	return 0;
}

static void motsol_sa_disconnect(struct usb_interface *interface)
{
	dev_dbg(&(interface)->dev, "disconnect\n");
}

#define MOTSOL_VENDOR_ID 0x0cad
#define MOTSOL_SA_PRODUCT_ID 0x01901

static struct usb_device_id motsol_sa_table[] = {
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 1) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 2) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 3) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 4) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 5) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 6) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 7) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 8) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 9) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 10) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 11) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 12) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 13) },
	{ USB_DEVICE(MOTSOL_VENDOR_ID, MOTSOL_SA_PRODUCT_ID + 14) },
	{}
};

MODULE_DEVICE_TABLE(usb, motsol_sa_table);

#ifdef CONFIG_PM
static int motsol_sa_suspend(struct usb_interface *interface,
			   pm_message_t message)
{
	dev_dbg(&(interface)->dev, "suspend");
	return 0;
}

static int motsol_sa_resume(struct usb_interface *interface)
{
	dev_dbg(&(interface)->dev, "resume");
	return 0;
}
#else
#define motsol_sa_suspend NULL
#define motsol_sa_resume NULL
#endif

static struct usb_driver motsol_sa_driver = {
	.name = "motsol_sa",
	.id_table = motsol_sa_table,
	.probe = motsol_sa_probe,
	.suspend = motsol_sa_suspend,
	.resume = motsol_sa_resume,
	.reset_resume = motsol_sa_resume,
	.supports_autosuspend = 1,
	.disconnect = motsol_sa_disconnect,
};

static int __init motsol_sa_init(void)
{
	int ret = -1;

	ret = usb_register(&motsol_sa_driver);
	pr_debug("%s: %s\n", KBUILD_MODNAME, DRIVER_DESC);
	return ret;
}

static void __exit motsol_sa_exit(void)
{
	usb_deregister(&motsol_sa_driver);
	pr_debug("%s driver deregistered\n", motsol_sa_driver.name);
}

module_init(motsol_sa_init);
module_exit(motsol_sa_exit);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
