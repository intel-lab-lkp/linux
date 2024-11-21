// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 MFD driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#define MFD_DEV_SIMPLE(_name)				\
{							\
	.name = NCT6694_DEV_##_name,			\
}							\

#define MFD_DEV_WITH_ID(_name, _id)			\
{							\
	.name = NCT6694_DEV_##_name,			\
	.id = _id,					\
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

	MFD_DEV_SIMPLE(HWMON),
	MFD_DEV_SIMPLE(RTC),
};

static int nct6694_response_err_handling(struct nct6694 *nct6694,
					 unsigned char err_status)
{
	struct device *dev = &nct6694->udev->dev;

	switch (err_status) {
	case NCT6694_NO_ERROR:
		return err_status;
	case NCT6694_NOT_SUPPORT_ERROR:
		dev_dbg(dev, "%s: Command is not support!\n", __func__);
		break;
	case NCT6694_NO_RESPONSE_ERROR:
		dev_dbg(dev, "%s: Command is no response!\n", __func__);
		break;
	case NCT6694_TIMEOUT_ERROR:
		dev_dbg(dev, "%s: Command is timeout!\n", __func__);
		break;
	case NCT6694_PENDING:
		dev_dbg(dev, "%s: Command is pending!\n", __func__);
		break;
	default:
		return -EINVAL;
	}

	return -EIO;
}

int nct6694_read_msg(struct nct6694 *nct6694, u8 mod, u16 offset,
		     u16 length, void *buf)
{
	struct nct6694_cmd_header *cmd_header = nct6694->cmd_header;
	struct nct6694_response_header *response_header = nct6694->response_header;
	struct usb_device *udev = nct6694->udev;
	int tx_len, rx_len, ret;

	guard(mutex)(&nct6694->access_lock);

	/* Send command packet to USB device */
	cmd_header->mod = mod;
	cmd_header->cmd = offset & 0xFF;
	cmd_header->sel = (offset >> 8) & 0xFF;
	cmd_header->hctrl = NCT6694_HCTRL_GET;
	cmd_header->len = length;

	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, NCT6694_BULK_OUT_EP),
			   cmd_header, NCT6694_CMD_PACKET_SZ, &tx_len,
			   nct6694->timeout);
	if (ret)
		return ret;

	/* Receive response packet from USB device */
	ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, NCT6694_BULK_IN_EP),
			   response_header, NCT6694_CMD_PACKET_SZ, &rx_len,
			   nct6694->timeout);
	if (ret)
		return ret;

	ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, NCT6694_BULK_IN_EP),
			   buf, NCT6694_MAX_PACKET_SZ, &rx_len, nct6694->timeout);
	if (ret)
		return ret;

	return nct6694_response_err_handling(nct6694, response_header->sts);
}
EXPORT_SYMBOL(nct6694_read_msg);

int nct6694_write_msg(struct nct6694 *nct6694, u8 mod, u16 offset,
		      u16 length, void *buf)
{
	struct nct6694_cmd_header *cmd_header = nct6694->cmd_header;
	struct nct6694_response_header *response_header = nct6694->response_header;
	struct usb_device *udev = nct6694->udev;
	int tx_len, rx_len, ret;

	guard(mutex)(&nct6694->access_lock);

	/* Send command packet to USB device  */
	cmd_header->mod = mod;
	cmd_header->cmd = offset & 0xFF;
	cmd_header->sel = (offset >> 8) & 0xFF;
	cmd_header->hctrl = NCT6694_HCTRL_SET;
	cmd_header->len = length;

	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, NCT6694_BULK_OUT_EP),
			   cmd_header, NCT6694_CMD_PACKET_SZ, &tx_len,
			   nct6694->timeout);
	if (ret)
		return ret;

	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, NCT6694_BULK_OUT_EP),
			   buf, length, &tx_len, nct6694->timeout);
	if (ret)
		return ret;

	/* Receive response packet from USB device */
	ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, NCT6694_BULK_IN_EP),
			   response_header, NCT6694_CMD_PACKET_SZ, &rx_len,
			   nct6694->timeout);
	if (ret)
		return ret;

	ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, NCT6694_BULK_IN_EP),
			   buf, NCT6694_MAX_PACKET_SZ, &rx_len, nct6694->timeout);
	if (ret)
		return ret;

	return nct6694_response_err_handling(nct6694, response_header->sts);
}
EXPORT_SYMBOL(nct6694_write_msg);

static void usb_int_callback(struct urb *urb)
{
	struct nct6694 *nct6694 = urb->context;
	struct device *dev = &nct6694->udev->dev;
	unsigned int *int_status = urb->transfer_buffer;
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

	while (*int_status) {
		int irq = __ffs(*int_status);

		if (*int_status & (1 << irq))
			generic_handle_irq_safe(irq_find_mapping(nct6694->domain, irq));

		*int_status &= ~(1 << irq);
	}

resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret)
		dev_dbg(dev, "%s: Failed to resubmit urb, status %d",
			__func__, ret);
}

static void nct6694_irq_lock(struct irq_data *data)
{
	struct nct6694 *nct6694 = irq_data_get_irq_chip_data(data);

	mutex_lock(&nct6694->irq_lock);
}

static void nct6694_irq_sync_unlock(struct irq_data *data)
{
	struct nct6694 *nct6694 = irq_data_get_irq_chip_data(data);

	mutex_unlock(&nct6694->irq_lock);
}

static void nct6694_irq_enable(struct irq_data *data)
{
	struct nct6694 *nct6694 = irq_data_get_irq_chip_data(data);
	unsigned int irq = data->hwirq;

	nct6694->irq_enable |= (1 << irq);
}

static void nct6694_irq_disable(struct irq_data *data)
{
	struct nct6694 *nct6694 = irq_data_get_irq_chip_data(data);
	unsigned int irq = data->hwirq;

	nct6694->irq_enable &= ~(1 << irq);
}

static struct irq_chip nct6694_irq_chip = {
	.name = "nct6694-irq",
	.flags = IRQCHIP_SKIP_SET_WAKE,
	.irq_bus_lock = nct6694_irq_lock,
	.irq_bus_sync_unlock = nct6694_irq_sync_unlock,
	.irq_enable = nct6694_irq_enable,
	.irq_disable = nct6694_irq_disable,
};

static int nct6694_irq_domain_map(struct irq_domain *d, unsigned int irq,
				  irq_hw_number_t hw)
{
	struct nct6694 *nct6694 = d->host_data;

	irq_set_chip_data(irq, nct6694);
	irq_set_chip_and_handler(irq, &nct6694_irq_chip, handle_simple_irq);

	return 0;
}

static void nct6694_irq_domain_unmap(struct irq_domain *d, unsigned int irq)
{
	irq_set_chip_and_handler(irq, NULL, NULL);
	irq_set_chip_data(irq, NULL);
}

static const struct irq_domain_ops nct6694_irq_domain_ops = {
	.map	= nct6694_irq_domain_map,
	.unmap	= nct6694_irq_domain_unmap,
};

static int nct6694_usb_probe(struct usb_interface *iface,
			     const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(iface);
	struct device *dev = &udev->dev;
	struct usb_host_interface *interface;
	struct usb_endpoint_descriptor *int_endpoint;
	struct nct6694 *nct6694;
	struct nct6694_cmd_header *cmd_header;
	struct nct6694_response_header *response_header;
	int pipe, maxp;
	int ret;

	interface = iface->cur_altsetting;

	int_endpoint = &interface->endpoint[0].desc;
	if (!usb_endpoint_is_int_in(int_endpoint))
		return -ENODEV;

	nct6694 = devm_kzalloc(dev, sizeof(*nct6694), GFP_KERNEL);
	if (!nct6694)
		return -ENOMEM;

	pipe = usb_rcvintpipe(udev, NCT6694_INT_IN_EP);
	maxp = usb_maxpacket(udev, pipe);

	cmd_header = devm_kzalloc(dev, sizeof(*cmd_header),
				  GFP_KERNEL);
	if (!cmd_header)
		return -ENOMEM;

	response_header = devm_kzalloc(dev, sizeof(*response_header),
				       GFP_KERNEL);
	if (!response_header)
		return -ENOMEM;

	nct6694->int_buffer = devm_kcalloc(dev, NCT6694_MAX_PACKET_SZ,
					   sizeof(unsigned char), GFP_KERNEL);
	if (!nct6694->int_buffer)
		return -ENOMEM;

	nct6694->int_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!nct6694->int_in_urb)
		return -ENOMEM;

	nct6694->domain = irq_domain_add_simple(NULL, NCT6694_NR_IRQS, 0,
						&nct6694_irq_domain_ops,
						nct6694);
	if (!nct6694->domain)
		return -ENODEV;

	nct6694->udev = udev;
	nct6694->timeout = NCT6694_URB_TIMEOUT;	/* Wait until urb complete */
	nct6694->cmd_header = cmd_header;
	nct6694->response_header = response_header;

	mutex_init(&nct6694->access_lock);
	mutex_init(&nct6694->irq_lock);

	usb_fill_int_urb(nct6694->int_in_urb, udev, pipe,
			 nct6694->int_buffer, maxp, usb_int_callback,
			 nct6694, int_endpoint->bInterval);
	ret = usb_submit_urb(nct6694->int_in_urb, GFP_KERNEL);
	if (ret)
		goto err_urb;

	dev_set_drvdata(dev, nct6694);
	usb_set_intfdata(iface, nct6694);

	ret = mfd_add_hotplug_devices(dev, nct6694_dev, ARRAY_SIZE(nct6694_dev));
	if (ret)
		goto err_mfd;

	dev_info(dev, "Probed device: (%04X:%04X)\n", id->idVendor, id->idProduct);
	return 0;

err_mfd:
	usb_kill_urb(nct6694->int_in_urb);
err_urb:
	usb_free_urb(nct6694->int_in_urb);
	return dev_err_probe(dev, ret, "Probe failed\n");
}

static void nct6694_usb_disconnect(struct usb_interface *iface)
{
	struct usb_device *udev = interface_to_usbdev(iface);
	struct nct6694 *nct6694 = usb_get_intfdata(iface);

	mfd_remove_devices(&udev->dev);
	usb_kill_urb(nct6694->int_in_urb);
	usb_free_urb(nct6694->int_in_urb);
}

static const struct usb_device_id nct6694_ids[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(NCT6694_VENDOR_ID,
					NCT6694_PRODUCT_ID,
					0xFF, 0x00, 0x00)},
	{}
};
MODULE_DEVICE_TABLE(usb, nct6694_ids);

static struct usb_driver nct6694_usb_driver = {
	.name	= "nct6694",
	.id_table = nct6694_ids,
	.probe = nct6694_usb_probe,
	.disconnect = nct6694_usb_disconnect,
};

module_usb_driver(nct6694_usb_driver);

MODULE_DESCRIPTION("USB-MFD driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
