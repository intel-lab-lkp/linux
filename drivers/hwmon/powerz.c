// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2023 Thomas Weißschuh <linux@weissschuh.net>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/usb.h>

#define DRIVER_NAME	"powerz"
#define POWERZ_EP_CMD_OUT	0x01
#define POWERZ_EP_DATA_IN	0x81

struct powerz_sensor_data {
	u8 _unknown_1[8];
	__le32 Vbus;
	__le32 Ibus;
	__le32 Vbus_avg;
	__le32 Ibus_avg;
	u8 _unknown_2[8];
	u8 temp[2];
	__le16 cc1;
	__le16 cc2;
	__le16 dp;
	__le16 dm;
	u8 _unknown_3[6];
} __packed;

struct powerz_priv {
	struct device *hwmon_dev;
	struct usb_interface *intf;
};

static const struct hwmon_channel_info * const powerz_info[] = {
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_AVERAGE,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_AVERAGE),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static umode_t powerz_is_visible(const void *data, enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	return 0444;
}

static int powerz_read_string(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, const char **str)
{
	if (type == hwmon_curr && attr == hwmon_curr_label)
		*str = "bus";
	else if (type == hwmon_in && attr == hwmon_in_label && channel == 0)
		*str = "bus";
	else if (type == hwmon_in && attr == hwmon_in_label && channel == 1)
		*str = "cc1";
	else if (type == hwmon_in && attr == hwmon_in_label && channel == 2)
		*str = "cc2";
	else if (type == hwmon_in && attr == hwmon_in_label && channel == 3)
		*str = "dp";
	else if (type == hwmon_in && attr == hwmon_in_label && channel == 4)
		*str = "dm";
	else if (type == hwmon_temp && attr == hwmon_temp_label)
		*str = "temp";
	else
		return -EINVAL;

	return 0;
}

struct powerz_usb_ctx {
	char transfer_buffer[64];
	struct completion completion;
	int status;
};

static void powerz_usb_data_complete(struct urb *urb)
{
	struct powerz_usb_ctx *ctx = urb->context;

	ctx->status = 0;
	complete(&ctx->completion);
}

static void powerz_usb_cmd_complete(struct urb *urb)
{
	struct powerz_usb_ctx *ctx = urb->context;
	int ret;

	usb_fill_bulk_urb(urb, urb->dev, usb_rcvbulkpipe(urb->dev, POWERZ_EP_DATA_IN),
			  ctx->transfer_buffer, sizeof(ctx->transfer_buffer),
			  powerz_usb_data_complete, ctx);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		ctx->status = ret;
		complete(&ctx->completion);
	}
}

static struct powerz_sensor_data *powerz_read_data(struct usb_device *udev,
						   struct powerz_usb_ctx *ctx)
{
	struct urb *urb;
	int r;

	ctx->status = -ETIMEDOUT;
	init_completion(&ctx->completion);

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return ERR_PTR(-ENOMEM);

	ctx->transfer_buffer[0] = 0x0c;
	ctx->transfer_buffer[1] = 0x00;
	ctx->transfer_buffer[2] = 0x02;
	ctx->transfer_buffer[3] = 0x00;

	usb_fill_bulk_urb(urb, udev, usb_sndbulkpipe(udev, POWERZ_EP_CMD_OUT),
			  ctx->transfer_buffer, 4, powerz_usb_cmd_complete, ctx);
	r = usb_submit_urb(urb, GFP_KERNEL);
	if (r) {
		usb_free_urb(urb);
		return ERR_PTR(r);
	}

	if (!wait_for_completion_interruptible_timeout(&ctx->completion, msecs_to_jiffies(5))) {
		usb_kill_urb(urb);
		usb_free_urb(urb);
		return ERR_PTR(-EIO);
	}

	if (ctx->status) {
		usb_free_urb(urb);
		return ERR_PTR(ctx->status);
	}

	if (urb->actual_length < (sizeof(struct powerz_sensor_data))) {
		usb_free_urb(urb);
		return ERR_PTR(-EIO);
	}

	usb_free_urb(urb);
	return (struct powerz_sensor_data *)(ctx->transfer_buffer);
}

static int powerz_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
		       int channel, long *val)
{
	struct usb_interface *intf = to_usb_interface(dev->parent);
	struct usb_device *udev = interface_to_usbdev(intf);
	struct powerz_sensor_data *data;
	struct powerz_usb_ctx *ctx;

	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	data = powerz_read_data(udev, ctx);
	if (IS_ERR(data)) {
		kfree(ctx);
		return PTR_ERR(data);
	}

	if (type == hwmon_curr && attr == hwmon_curr_input)
		*val =  ((s32)le32_to_cpu(data->Ibus)) / 1000;
	else if (type == hwmon_curr && attr == hwmon_curr_average)
		*val =  ((s32)le32_to_cpu(data->Ibus_avg)) / 1000;
	else if (type == hwmon_in && attr == hwmon_in_input && channel == 0)
		*val =  le32_to_cpu(data->Vbus) / 1000;
	else if (type == hwmon_in && attr == hwmon_in_average && channel == 0)
		*val =  le32_to_cpu(data->Vbus_avg) / 1000;
	else if (type == hwmon_in && attr == hwmon_in_input && channel == 1)
		*val =  le16_to_cpu(data->cc1) / 10;
	else if (type == hwmon_in && attr == hwmon_in_input && channel == 2)
		*val =  le16_to_cpu(data->cc2) / 10;
	else if (type == hwmon_in && attr == hwmon_in_input && channel == 3)
		*val =  le16_to_cpu(data->dp) / 10;
	else if (type == hwmon_in && attr == hwmon_in_input && channel == 4)
		*val =  le16_to_cpu(data->dm) / 10;
	else if (type == hwmon_temp && attr == hwmon_temp_input)
		*val = ((long)data->temp[1]) * 2000 + ((long)data->temp[0]) * 1000 / 128;
	else
		return -EINVAL;

	kfree(ctx);

	return 0;
}

static const struct hwmon_ops powerz_hwmon_ops = {
	.is_visible  = powerz_is_visible,
	.read        = powerz_read,
	.read_string = powerz_read_string,
};

static const struct hwmon_chip_info powerz_chip_info = {
	.ops  = &powerz_hwmon_ops,
	.info = powerz_info,
};

static int powerz_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct powerz_priv *priv;
	struct device *parent;
	const char *name;
	int ret;

	parent = &intf->dev;

	priv = devm_kzalloc(parent, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	name = devm_hwmon_sanitize_name(parent, udev->product ?: DRIVER_NAME);
	priv->hwmon_dev = hwmon_device_register_with_info(parent, name,
							  priv,
							  &powerz_chip_info,
							  NULL);
	ret = PTR_ERR_OR_ZERO(priv->hwmon_dev);
	priv->intf = intf;
	usb_set_intfdata(intf, priv);

	return ret;
}

static void powerz_disconnect(struct usb_interface *intf)
{
	struct powerz_priv *priv = usb_get_intfdata(intf);

	hwmon_device_unregister(priv->hwmon_dev);
}

static const struct usb_device_id powerz_id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x5FC9, 0x0063, 0x00) }, /* ChargerLAB POWER-Z KM003C */
	{ }
};
MODULE_DEVICE_TABLE(usb, powerz_id_table);

static struct usb_driver powerz_driver = {
	.name       = DRIVER_NAME,
	.id_table   = powerz_id_table,
	.probe      = powerz_probe,
	.disconnect = powerz_disconnect,
};
module_usb_driver(powerz_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Thomas Weißschuh <linux@weissschuh.net>");
MODULE_DESCRIPTION("ChargerLAB POWER-Z USB-C tester");
