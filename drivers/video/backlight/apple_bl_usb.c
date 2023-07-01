// SPDX-License-Identifier: GPL-2.0 OR MIT
#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/backlight.h>
#include <asm/byteorder.h>

#define APPLE_STUDIO_DISPLAY_VENDOR_ID  0x05ac
#define APPLE_STUDIO_DISPLAY_PRODUCT_ID 0x1114

#define HID_GET_REPORT 0x01
#define HID_SET_REPORT 0x09

#define HID_REPORT_TYPE_FEATURE 0x0300

struct apple_bl_usb_data {
	struct usb_interface *usb_interface;
	struct usb_device *usb_dev;
};

struct brightness_ctrl_message_data {
	u8 unknown_1;
	__le16 brightness;
	u8 unkown_2[4];
} __packed;

void init_ctrl_msg_data(struct brightness_ctrl_message_data *msg)
{
	memset(msg, 0, sizeof(struct brightness_ctrl_message_data));
	msg->unknown_1 = 0x01;
}

void set_ctrl_message_brightness(struct brightness_ctrl_message_data *msg,
				 u16 brightness_value)
{
	msg->brightness = cpu_to_le16(brightness_value + 400);
}

u16 get_ctrl_message_brightness(struct brightness_ctrl_message_data *msg)
{
	return le16_to_cpu(msg->brightness) - 400;
}

int apple_bl_usb_usb_get_brightness(struct usb_interface *interface,
				    struct usb_device *usb_dev,
				    int *brightness)
{
	int err;
	u16 interface_nr;
	int msg_data_size;
	struct brightness_ctrl_message_data *msg_data;

	msg_data_size = sizeof(struct brightness_ctrl_message_data);
	msg_data = kzalloc(msg_data_size, GFP_KERNEL);
	memset(msg_data, 0x00, msg_data_size);
	interface_nr = interface->cur_altsetting->desc.bInterfaceNumber;

	err = usb_control_msg(usb_dev,
			      usb_rcvctrlpipe(usb_dev, 0),
			      HID_GET_REPORT,
			      USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			      /* wValue: HID-Report Type and Report ID */
			      HID_REPORT_TYPE_FEATURE | 0x01,
			      interface_nr /* wIndex */,
			      msg_data,
			      msg_data_size,
			      HZ);
	if (err < 0) {
		dev_err(&interface->dev,
			"get: usb control message err: %d\n",
			err);
	}
	*brightness = get_ctrl_message_brightness(msg_data);
	kfree(msg_data);
	dev_dbg(&interface->dev, "get brightness: %d\n", *brightness);
	return 0;
}

int apple_bl_usb_usb_set_brightness(struct usb_interface *interface,
				    struct usb_device *usb_dev,
				    int brightness)
{
	int err;
	u16 interface_nr;
	struct brightness_ctrl_message_data *msg_data;

	msg_data = kzalloc(sizeof(struct brightness_ctrl_message_data), GFP_KERNEL);
	interface_nr = interface->cur_altsetting->desc.bInterfaceNumber;
	init_ctrl_msg_data(msg_data);
	set_ctrl_message_brightness(msg_data, brightness);

	err = usb_control_msg(usb_dev,
			      usb_sndctrlpipe(usb_dev, 0),
			      HID_SET_REPORT,
			      USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			      /* wValue: HID-Report Type and Report ID */
			      HID_REPORT_TYPE_FEATURE | 0x01,
			      interface_nr /* wIndex */,
			      msg_data,
			      sizeof(struct brightness_ctrl_message_data),
			      HZ);
	kfree(msg_data);
	if (err < 0) {
		dev_err(&interface->dev,
			"set: usb control message err: %d\n",
			err);
		return err;
	}
	dev_dbg(&interface->dev, "set brightness: %d\n", brightness);
	return 0;
}

int apple_bl_usb_check_fb(struct backlight_device *bd, struct fb_info *info)
{
	dev_info(&bd->dev, "check fb\n");
	return 0;
}

int apple_bl_usb_get_brightness(struct backlight_device *bl)
{
	int ret;
	struct apple_bl_usb_data *data;
	int hw_brightness;

	data = bl_get_data(bl);
	ret = apple_bl_usb_usb_get_brightness(data->usb_interface,
					      data->usb_dev,
					      &hw_brightness);
	if (!ret)
		ret = hw_brightness;

	return ret;
}

int apple_bl_usb_update_status(struct backlight_device *bl)
{
	int err;
	struct apple_bl_usb_data *data;

	data = bl_get_data(bl);
	err = apple_bl_usb_usb_set_brightness(data->usb_interface,
					      data->usb_dev,
					      bl->props.brightness);
	return err;
}

static const struct backlight_ops apple_bl_usb_ops = {
	.update_status  = apple_bl_usb_update_status,
	.get_brightness = apple_bl_usb_get_brightness,
	.check_fb       = apple_bl_usb_check_fb,
};

static void apple_bl_usb_disconnect(struct usb_interface *interface)
{
	struct backlight_device *bl;

	dev_dbg(&interface->dev, "disconnect\n");

	bl = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);
	backlight_device_unregister(bl);
}

static int apple_bl_usb_probe(struct usb_interface *interface,
			      const struct usb_device_id *id)
{
	struct backlight_properties props;
	struct backlight_device *bl;
	struct usb_device *usb_dev;
	struct device *dev;
	struct apple_bl_usb_data *data;
	int brightness_interface_nr;

	dev_dbg(&interface->dev, "probe\n");

	dev = &interface->dev;
	usb_dev = interface_to_usbdev(interface);

	switch (usb_dev->config->desc.bConfigurationValue) {
	case 1:
		brightness_interface_nr = 0x7;
		break;
	case 2:
		brightness_interface_nr = 0x9;
		break;
	case 3:
		brightness_interface_nr = 0xc;
		break;
	default:
		dev_err(dev,
			"unexpected configuration value: %d\n",
			usb_dev->config->desc.bConfigurationValue);
		return -EINVAL;
	}

	if (interface->cur_altsetting->desc.bInterfaceNumber != brightness_interface_nr)
		return -ENODEV;

	data = devm_kzalloc(dev,
			    sizeof(struct apple_bl_usb_data),
			    GFP_KERNEL);
	if (IS_ERR(data)) {
		dev_err(dev, "failed to allocate memory\n");
		return PTR_ERR(bl);
	}
	data->usb_interface = interface;
	data->usb_dev = usb_dev;

	// Valid brightness values for the apple studio display range from 400
	// to 60000. Since the backlight subsystem´s brightness value starts
	// from 0, we use 0 to 59600 and offset it by the minimum value.
	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = 59600;

	bl = backlight_device_register("apple_studio_display",
				       dev,
				       data,
				       &apple_bl_usb_ops,
				       &props);
	if (IS_ERR(bl)) {
		dev_err(dev, "failed to register backlight\n");
		return PTR_ERR(bl);
	}
	usb_set_intfdata(interface, bl);
	return 0;
}

static int apple_bl_usb_suspend(struct usb_interface *interface,
				pm_message_t message)
{
	dev_dbg(&interface->dev, "suspend\n");
	return 0;
}

static int apple_bl_usb_resume(struct usb_interface *interface)
{
	dev_dbg(&interface->dev, "resume\n");
	return 0;
}

static const struct usb_device_id id_table[] = {
	{
		.idVendor    = APPLE_STUDIO_DISPLAY_VENDOR_ID,
		.idProduct   = APPLE_STUDIO_DISPLAY_PRODUCT_ID,
	},
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver usb_asdbl_driver = {
	.name         = "apple_bl_usb",
	.probe        = apple_bl_usb_probe,
	.disconnect   = apple_bl_usb_disconnect,
	.id_table     = id_table,
	.suspend      = apple_bl_usb_suspend,
	.resume       = apple_bl_usb_resume,
	.reset_resume = apple_bl_usb_resume
};
module_usb_driver(usb_asdbl_driver);

MODULE_AUTHOR("Julius Zint <julius@zint.sh>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Backlight control for USB-C Thunderbolt Apple displays");
