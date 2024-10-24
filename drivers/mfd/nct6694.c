// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 MFD driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/io.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>

#define DRVNAME "nct6694-usb_mfd"

#define MFD_DEV_SIMPLE(_name)		\
{					\
	.name = NCT6694_DEV_##_name,	\
}					\

#define MFD_DEV_WITH_ID(_name, _id)	\
{					\
	.name = NCT6694_DEV_##_name,	\
	.id = _id,			\
}

/* MFD device resources */
static const struct mfd_cell nct6694_dev[] = {
	MFD_DEV_WITH_ID(GPIO, 0x0),
	MFD_DEV_WITH_ID(GPIO, 0x1),
	MFD_DEV_WITH_ID(GPIO, 0x2),
	MFD_DEV_WITH_ID(GPIO, 0x3),
	MFD_DEV_WITH_ID(GPIO, 0x4),
	MFD_DEV_WITH_ID(GPIO, 0x5),
	MFD_DEV_WITH_ID(GPIO, 0x6),
	MFD_DEV_WITH_ID(GPIO, 0x7),
	MFD_DEV_WITH_ID(GPIO, 0x8),
	MFD_DEV_WITH_ID(GPIO, 0x9),
	MFD_DEV_WITH_ID(GPIO, 0xA),
	MFD_DEV_WITH_ID(GPIO, 0xB),
	MFD_DEV_WITH_ID(GPIO, 0xC),
	MFD_DEV_WITH_ID(GPIO, 0xD),
	MFD_DEV_WITH_ID(GPIO, 0xE),
	MFD_DEV_WITH_ID(GPIO, 0xF),

	MFD_DEV_WITH_ID(I2C, 0x0),
	MFD_DEV_WITH_ID(I2C, 0x1),
	MFD_DEV_WITH_ID(I2C, 0x2),
	MFD_DEV_WITH_ID(I2C, 0x3),
	MFD_DEV_WITH_ID(I2C, 0x4),
	MFD_DEV_WITH_ID(I2C, 0x5),

	MFD_DEV_WITH_ID(CAN, 0x0),
	MFD_DEV_WITH_ID(CAN, 0x1),

	MFD_DEV_WITH_ID(WDT, 0x0),
	MFD_DEV_WITH_ID(WDT, 0x1),

	MFD_DEV_SIMPLE(IIO),
	MFD_DEV_SIMPLE(HWMON),
	MFD_DEV_SIMPLE(PWM),
	MFD_DEV_SIMPLE(RTC),
};

int nct6694_register_handler(struct nct6694 *nct6694, int irq_bit,
			     void (*handler)(void *), void *private_data)
{
	struct nct6694_handler_entry *entry;
	unsigned long flags;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->irq_bit = irq_bit;
	entry->handler = handler;
	entry->private_data = private_data;

	spin_lock_irqsave(&nct6694->lock, flags);
	list_add_tail(&entry->list, &nct6694->handler_list);
	spin_unlock_irqrestore(&nct6694->lock, flags);

	return 0;
}
EXPORT_SYMBOL(nct6694_register_handler);

int nct6694_read_msg(struct nct6694 *nct6694, u8 mod, u16 offset, u16 length,
		     u8 rd_idx, u8 rd_len, unsigned char *buf)
{
	struct usb_device *udev = nct6694->udev;
	unsigned char err_status;
	int len, packet_len, tx_len, rx_len;
	int i, ret;

	mutex_lock(&nct6694->access_lock);

	/* Send command packet to USB device */
	nct6694->cmd_buffer[REQUEST_MOD_IDX] = mod;
	nct6694->cmd_buffer[REQUEST_CMD_IDX] = offset & 0xFF;
	nct6694->cmd_buffer[REQUEST_SEL_IDX] = (offset >> 8) & 0xFF;
	nct6694->cmd_buffer[REQUEST_HCTRL_IDX] = HCTRL_GET;
	nct6694->cmd_buffer[REQUEST_LEN_L_IDX] = length & 0xFF;
	nct6694->cmd_buffer[REQUEST_LEN_H_IDX] = (length >> 8) & 0xFF;

	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, BULK_OUT_ENDPOINT),
			   nct6694->cmd_buffer, CMD_PACKET_SZ, &tx_len,
			   nct6694->timeout);
	if (ret)
		goto err;

	/* Receive response packet from USB device */
	ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, BULK_IN_ENDPOINT),
			   nct6694->rx_buffer, CMD_PACKET_SZ, &rx_len,
			   nct6694->timeout);
	if (ret)
		goto err;

	err_status = nct6694->rx_buffer[RESPONSE_STS_IDX];

	/*
	 * Segmented reception of messages that exceed the size of USB bulk
	 * pipe packets.
	 */
	for (i = 0, len = length; len > 0; i++, len -= packet_len) {
		if (len > nct6694->maxp)
			packet_len = nct6694->maxp;
		else
			packet_len = len;

		ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, BULK_IN_ENDPOINT),
				   nct6694->rx_buffer + nct6694->maxp * i,
				   packet_len, &rx_len, nct6694->timeout);
		if (ret)
			goto err;
	}

	for (i = 0; i < rd_len; i++)
		buf[i] = nct6694->rx_buffer[i + rd_idx];

	if (err_status) {
		pr_debug("%s: MSG CH status = %2Xh\n", __func__, err_status);
		ret = -EIO;
	}

err:
	mutex_unlock(&nct6694->access_lock);
	return ret;
}
EXPORT_SYMBOL(nct6694_read_msg);

int nct6694_write_msg(struct nct6694 *nct6694, u8 mod, u16 offset,
		      u16 length, unsigned char *buf)
{
	struct usb_device *udev = nct6694->udev;
	unsigned char err_status;
	int len, packet_len, tx_len, rx_len;
	int i, ret;

	mutex_lock(&nct6694->access_lock);

	/* Send command packet to USB device  */
	nct6694->cmd_buffer[REQUEST_MOD_IDX] = mod;
	nct6694->cmd_buffer[REQUEST_CMD_IDX] = offset & 0xFF;
	nct6694->cmd_buffer[REQUEST_SEL_IDX] = (offset >> 8) & 0xFF;
	nct6694->cmd_buffer[REQUEST_HCTRL_IDX] = HCTRL_SET;
	nct6694->cmd_buffer[REQUEST_LEN_L_IDX] = length & 0xFF;
	nct6694->cmd_buffer[REQUEST_LEN_H_IDX] = (length >> 8) & 0xFF;

	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, BULK_OUT_ENDPOINT),
			   nct6694->cmd_buffer, CMD_PACKET_SZ, &tx_len,
			   nct6694->timeout);
	if (ret)
		goto err;

	/*
	 * Segmented transmission of messages that exceed the size of USB bulk
	 * pipe packets.
	 */
	for (i = 0, len = length; len > 0; i++, len -= packet_len) {
		if (len > nct6694->maxp)
			packet_len = nct6694->maxp;
		else
			packet_len = len;

		memcpy(nct6694->tx_buffer + nct6694->maxp * i,
		       buf + nct6694->maxp * i, packet_len);

		ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, BULK_OUT_ENDPOINT),
				   nct6694->tx_buffer + nct6694->maxp * i,
				   packet_len, &tx_len, nct6694->timeout);
		if (ret)
			goto err;
	}

	/* Receive response packet from USB device */
	ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, BULK_IN_ENDPOINT),
			   nct6694->rx_buffer, CMD_PACKET_SZ, &rx_len,
			   nct6694->timeout);
	if (ret)
		goto err;

	err_status = nct6694->rx_buffer[RESPONSE_STS_IDX];

	/*
	 * Segmented reception of messages that exceed the size of USB bulk
	 * pipe packets.
	 */
	for (i = 0, len = length; len > 0; i++, len -= packet_len) {
		if (len > nct6694->maxp)
			packet_len = nct6694->maxp;
		else
			packet_len = len;

		ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, BULK_IN_ENDPOINT),
				   nct6694->rx_buffer + nct6694->maxp * i,
				   packet_len, &rx_len, nct6694->timeout);
		if (ret)
			goto err;
	}

	memcpy(buf, nct6694->rx_buffer, length);

	if (err_status) {
		pr_debug("%s: MSG CH status = %2Xh\n", __func__, err_status);
		ret = -EIO;
	}

err:
	mutex_unlock(&nct6694->access_lock);
	return ret;
}
EXPORT_SYMBOL(nct6694_write_msg);

static void usb_int_callback(struct urb *urb)
{
	unsigned char *int_status = urb->transfer_buffer;
	struct nct6694 *nct6694 = urb->context;
	struct nct6694_handler_entry *entry;
	unsigned long flags;
	int ret;

	switch (urb->status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	default:
		goto resubmit;
	}

	spin_lock_irqsave(&nct6694->lock, flags);

	list_for_each_entry(entry, &nct6694->handler_list, list) {
		if (int_status[INT_IN_IRQ_IDX] & entry->irq_bit)
			entry->handler(entry->private_data);
	}

	spin_unlock_irqrestore(&nct6694->lock, flags);

resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret)
		pr_debug("%s: Failed to resubmit urb, status %d",
			 __func__, ret);
}

static int nct6694_usb_probe(struct usb_interface *iface,
			     const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(iface);
	struct device *dev = &udev->dev;
	struct usb_host_interface *interface;
	struct usb_endpoint_descriptor *int_endpoint;
	struct nct6694 *nct6694;
	int pipe, maxp, bulk_pipe;
	int ret = EINVAL;

	interface = iface->cur_altsetting;
	/* Binding interface class : 0xFF */
	if (interface->desc.bInterfaceClass != USB_CLASS_VENDOR_SPEC ||
	    interface->desc.bInterfaceSubClass != 0x00 ||
	    interface->desc.bInterfaceProtocol != 0x00)
		return -ENODEV;

	int_endpoint = &interface->endpoint[0].desc;
	if (!usb_endpoint_is_int_in(int_endpoint))
		return -ENODEV;

	nct6694 = devm_kzalloc(&udev->dev, sizeof(*nct6694), GFP_KERNEL);
	if (!nct6694)
		return -ENOMEM;

	pipe = usb_rcvintpipe(udev, INT_IN_ENDPOINT);
	maxp = usb_maxpacket(udev, pipe);

	nct6694->cmd_buffer = devm_kcalloc(dev, CMD_PACKET_SZ,
					   sizeof(unsigned char), GFP_KERNEL);
	if (!nct6694->cmd_buffer)
		return -ENOMEM;
	nct6694->rx_buffer = devm_kcalloc(dev, MAX_PACKET_SZ,
					  sizeof(unsigned char), GFP_KERNEL);
	if (!nct6694->rx_buffer)
		return -ENOMEM;
	nct6694->tx_buffer = devm_kcalloc(dev, MAX_PACKET_SZ,
					  sizeof(unsigned char), GFP_KERNEL);
	if (!nct6694->tx_buffer)
		return -ENOMEM;
	nct6694->int_buffer = devm_kcalloc(dev, MAX_PACKET_SZ,
					   sizeof(unsigned char), GFP_KERNEL);
	if (!nct6694->int_buffer)
		return -ENOMEM;

	nct6694->int_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!nct6694->int_in_urb) {
		dev_err(&udev->dev, "Failed to allocate INT-in urb!\n");
		return -ENOMEM;
	}

	/* Bulk pipe maximum packet for each transaction */
	bulk_pipe = usb_sndbulkpipe(udev, BULK_OUT_ENDPOINT);
	nct6694->maxp = usb_maxpacket(udev, bulk_pipe);

	mutex_init(&nct6694->access_lock);
	nct6694->udev = udev;
	nct6694->timeout = URB_TIMEOUT;	/* Wait until urb complete */

	INIT_LIST_HEAD(&nct6694->handler_list);
	spin_lock_init(&nct6694->lock);

	usb_fill_int_urb(nct6694->int_in_urb, udev, pipe,
			 nct6694->int_buffer, maxp, usb_int_callback,
			 nct6694, int_endpoint->bInterval);
	ret = usb_submit_urb(nct6694->int_in_urb, GFP_KERNEL);
	if (ret)
		goto err_urb;

	dev_set_drvdata(&udev->dev, nct6694);
	usb_set_intfdata(iface, nct6694);

	ret = mfd_add_hotplug_devices(&udev->dev, nct6694_dev,
				      ARRAY_SIZE(nct6694_dev));
	if (ret) {
		dev_err(&udev->dev, "Failed to add mfd's child device\n");
		goto err_mfd;
	}

	nct6694->async_workqueue = alloc_ordered_workqueue("asyn_workqueue", 0);

	dev_info(&udev->dev, "Probed device: (%04X:%04X)\n",
		 id->idVendor, id->idProduct);
	return 0;

err_mfd:
	usb_kill_urb(nct6694->int_in_urb);
err_urb:
	usb_free_urb(nct6694->int_in_urb);
	return ret;
}

static void nct6694_usb_disconnect(struct usb_interface *iface)
{
	struct usb_device *udev = interface_to_usbdev(iface);
	struct nct6694 *nct6694 = usb_get_intfdata(iface);

	mfd_remove_devices(&udev->dev);
	flush_workqueue(nct6694->async_workqueue);
	destroy_workqueue(nct6694->async_workqueue);
	usb_set_intfdata(iface, NULL);
	usb_kill_urb(nct6694->int_in_urb);
	usb_free_urb(nct6694->int_in_urb);
}

static const struct usb_device_id nct6694_ids[] = {
	{ USB_DEVICE(NCT6694_VENDOR_ID, NCT6694_PRODUCT_ID)},
	{},
};
MODULE_DEVICE_TABLE(usb, nct6694_ids);

static struct usb_driver nct6694_usb_driver = {
	.name	= DRVNAME,
	.id_table = nct6694_ids,
	.probe = nct6694_usb_probe,
	.disconnect = nct6694_usb_disconnect,
};

module_usb_driver(nct6694_usb_driver);

MODULE_DESCRIPTION("USB-MFD driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
