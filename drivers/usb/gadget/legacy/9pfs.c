// SPDX-License-Identifier: GPL-2.0+
/*
 * usb9pfs.c -- Gadget usb9pfs
 *
 * Copyright (C) 2023 Michael Grzeschik
 */

/*
 * Gadget usb9pfs only needs two bulk endpoints, and will use the usb9pfs usb
 * transport to mount host filesystem via usb gadget.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/usb/composite.h>
#include <linux/netdevice.h>

#include "u_eem.h"
#include "u_ether.h"

/*-------------------------------------------------------------------------*/
USB_GADGET_COMPOSITE_OPTIONS();

USB_ETHERNET_MODULE_PARAMETERS();

/* Defines */

#define DRIVER_VERSION_STR "v1.0"
#define DRIVER_VERSION_NUM 0x1000

#define DRIVER_DESC	"Composite Gadget (9P + ACM + NCM)"

/*-------------------------------------------------------------------------*/

#define DRIVER_VENDOR_NUM	0x1d6b		/* Linux Foundation */
#define DRIVER_PRODUCT_NUM	0x0109		/* Linux-USB 9PFS Gadget */

/*-------------------------------------------------------------------------*/

static struct usb_device_descriptor device_desc = {
	.bLength =		sizeof(device_desc),
	.bDescriptorType =	USB_DT_DEVICE,

	/* .bcdUSB = DYNAMIC */

	.bDeviceClass =		USB_CLASS_MISC,
	.bDeviceSubClass =	2,
	.bDeviceProtocol =	1,

	/* .bMaxPacketSize0 = f(hardware) */

	/* Vendor and product id can be overridden by module parameters.  */
	.idVendor =		cpu_to_le16(DRIVER_VENDOR_NUM),
	.idProduct =		cpu_to_le16(DRIVER_PRODUCT_NUM),
	/* .bcdDevice = f(hardware) */
	/* .iManufacturer = DYNAMIC */
	/* .iProduct = DYNAMIC */
	/* NO SERIAL NUMBER */
	/*.bNumConfigurations =	DYNAMIC*/
};

static const struct usb_descriptor_header *otg_desc[2];

static struct usb_string strings_dev[] = {
	[USB_GADGET_MANUFACTURER_IDX].s = "",
	[USB_GADGET_PRODUCT_IDX].s = DRIVER_DESC,
	[USB_GADGET_SERIAL_IDX].s = "",
	{  }			/* end of list */
};

static struct usb_gadget_strings stringtab_dev = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings_dev,
};

static struct usb_gadget_strings *dev_strings[] = {
	&stringtab_dev,
	NULL,
};

static struct usb_configuration cdc_driver_conf = {
	.label          = DRIVER_DESC,
	.bConfigurationValue = 1,
	/* .iConfiguration = DYNAMIC */
	.bmAttributes   = USB_CONFIG_ATT_SELFPOWER,
};

static struct usb_function *f_9pfs;
static struct usb_function_instance *fi_9pfs;

static struct usb_function *f_acm;
static struct usb_function_instance *fi_acm;

static struct usb_function *f_eem;
static struct usb_function_instance *fi_eem;

static int cdc_do_config(struct usb_configuration *c)
{
	int ret;

	if (gadget_is_otg(c->cdev->gadget)) {
		c->descriptors = otg_desc;
		c->bmAttributes |= USB_CONFIG_ATT_WAKEUP;
	}

	f_9pfs = usb_get_function(fi_9pfs);
	if (IS_ERR(f_9pfs))
		return PTR_ERR(f_9pfs);

	ret = usb_add_function(c, f_9pfs);
	if (ret < 0)
		goto err_func_9pfs;

	f_acm = usb_get_function(fi_acm);
	if (IS_ERR(f_acm)) {
		ret = PTR_ERR(f_acm);
		goto err_func_acm;
	}

	ret = usb_add_function(c, f_acm);
	if (ret)
		goto err_conf;

	f_eem = usb_get_function(fi_eem);
	if (IS_ERR(f_eem)) {
		ret = PTR_ERR(f_eem);
		goto err_eem;
	}

	ret = usb_add_function(c, f_eem);
	if (ret)
		goto err_run;

	return 0;
err_run:
	usb_put_function(f_eem);
err_eem:
	usb_remove_function(c, f_acm);
err_conf:
	usb_put_function(f_acm);
err_func_acm:
	usb_remove_function(c, f_9pfs);
err_func_9pfs:
	usb_put_function(f_9pfs);
	return ret;
}

static int usb9pfs_bind(struct usb_composite_dev *cdev)
{
	struct f_eem_opts	*eem_opts = NULL;
	int status;

	fi_9pfs = usb_get_function_instance("usb9pfs");
	if (IS_ERR(fi_9pfs))
		return PTR_ERR(fi_9pfs);

	/* set up serial link layer */
	fi_acm = usb_get_function_instance("acm");
	if (IS_ERR(fi_acm)) {
		status = PTR_ERR(fi_acm);
		goto err_conf_acm;
	}

	fi_eem = usb_get_function_instance("eem");
	if (IS_ERR(fi_eem)) {
		status = PTR_ERR(fi_eem);
		goto err_conf_eem;
	}

	eem_opts = container_of(fi_eem, struct f_eem_opts, func_inst);

	gether_set_qmult(eem_opts->net, qmult);
	if (!gether_set_host_addr(eem_opts->net, host_addr))
		pr_info("using host ethernet address: %s", host_addr);
	if (!gether_set_dev_addr(eem_opts->net, dev_addr))
		pr_info("using self ethernet address: %s", dev_addr);

	/* Allocate string descriptor numbers ... note that string
	 * contents can be overridden by the composite_dev glue.
	 */
	status = usb_string_ids_tab(cdev, strings_dev);
	if (status < 0)
		return status;

	device_desc.iManufacturer = strings_dev[USB_GADGET_MANUFACTURER_IDX].id;
	device_desc.iProduct = strings_dev[USB_GADGET_PRODUCT_IDX].id;
	device_desc.iSerialNumber = strings_dev[USB_GADGET_SERIAL_IDX].id;

	/* support OTG systems */
	if (gadget_is_otg(cdev->gadget)) {
		if (!otg_desc[0]) {
			struct usb_descriptor_header *usb_desc;

			usb_desc = usb_otg_descriptor_alloc(cdev->gadget);
			if (!usb_desc) {
				status = -ENOMEM;
				goto err_conf_otg;
			}
			usb_otg_descriptor_init(cdev->gadget, usb_desc);
			otg_desc[0] = usb_desc;
			otg_desc[1] = NULL;
		}
	}

	status = usb_add_config(cdev, &cdc_driver_conf, cdc_do_config);
	if (status)
		goto err_free_otg_desc;

	usb_ep_autoconfig_reset(cdev->gadget);
	usb_composite_overwrite_options(cdev, &coverwrite);

	dev_info(&cdev->gadget->dev, DRIVER_DESC " version: " DRIVER_VERSION_STR "\n");

	return 0;

err_free_otg_desc:
	kfree(otg_desc[0]);
	otg_desc[0] = NULL;
err_conf_otg:
	usb_put_function_instance(fi_eem);
err_conf_eem:
	usb_put_function_instance(fi_acm);
err_conf_acm:
	usb_put_function_instance(fi_9pfs);
	return status;
}

static int usb9pfs_unbind(struct usb_composite_dev *cdev)
{
	if (!IS_ERR_OR_NULL(f_eem))
		usb_put_function(f_eem);
	usb_put_function_instance(fi_eem);
	if (!IS_ERR_OR_NULL(f_acm))
		usb_put_function(f_acm);
	usb_put_function_instance(fi_acm);
	if (!IS_ERR_OR_NULL(f_9pfs))
		usb_put_function(f_9pfs);
	usb_put_function_instance(fi_9pfs);
	kfree(otg_desc[0]);
	otg_desc[0] = NULL;

	return 0;
}

static struct usb_composite_driver usb9pfs_driver = {
	.name		= "usb9pfs",
	.dev		= &device_desc,
	.strings	= dev_strings,
	.max_speed	= USB_SPEED_SUPER,
	.bind		= usb9pfs_bind,
	.unbind		= usb9pfs_unbind,
};

module_usb_composite_driver(usb9pfs_driver);

MODULE_AUTHOR("Michael Grzeschik");
MODULE_LICENSE("GPL");
