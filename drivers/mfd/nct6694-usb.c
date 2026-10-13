// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Nuvoton Technology Corp.
 *
 * Nuvoton NCT6694 core driver using USB interface to provide
 * access to the NCT6694 hardware monitoring and control features.
 *
 * The NCT6694 is an integrated controller that provides GPIO, I2C,
 * CAN, WDT, HWMON and RTC management.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/usb.h>

#define NCT6694_VENDOR_ID	0x0416
#define NCT6694_PRODUCT_ID	0x200B
#define NCT6694_INT_IN_EP	0x01
#define NCT6694_BULK_IN_EP	0x02
#define NCT6694_BULK_OUT_EP	0x03

#define NCT6694_URB_TIMEOUT	1000

union __packed nct6694_usb_hdr {
	struct nct6694_cmd_header cmd_header;
	struct nct6694_response_header response_header;
};

struct nct6694_usb_data {
	struct nct6694 core;
	struct usb_device *usb_dev;
	struct urb *int_urb;
	union nct6694_usb_hdr *hdr_buf;
	void *data_buf;
	__le32 *int_buf;
};

static struct nct6694_usb_data *to_nct6694_usb_data(struct nct6694 *ddata)
{
	return container_of(ddata, struct nct6694_usb_data, core);
}

static const struct mfd_cell nct6694_usb_devs[] = {
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),

	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),

	MFD_CELL_NAME("nct6694-canfd"),
	MFD_CELL_NAME("nct6694-canfd"),

	MFD_CELL_NAME("nct6694-wdt"),
	MFD_CELL_NAME("nct6694-wdt"),

	MFD_CELL_NAME("nct6694-hwmon"),

	MFD_CELL_NAME("nct6694-rtc"),
};

static int nct6694_usb_err_handling(struct nct6694 *ddata, unsigned char err_status)
{
	switch (err_status) {
	case NCT6694_NO_ERROR:
		return 0;
	case NCT6694_NOT_SUPPORT_ERROR:
		dev_err(ddata->dev, "Command is not supported!\n");
		break;
	case NCT6694_NO_RESPONSE_ERROR:
		dev_warn(ddata->dev, "Command received no response!\n");
		break;
	case NCT6694_TIMEOUT_ERROR:
		dev_warn(ddata->dev, "Command timed out!\n");
		break;
	case NCT6694_PENDING:
		dev_err(ddata->dev, "Command is pending!\n");
		break;
	default:
		return -EINVAL;
	}

	return -EIO;
}

static int nct6694_usb_read_msg(struct nct6694 *ddata,
				const struct nct6694_cmd_header *cmd_hd,
				void *rx_buf)
{
	struct nct6694_usb_data *usb_data = to_nct6694_usb_data(ddata);
	union nct6694_usb_hdr *hdr = usb_data->hdr_buf;
	struct usb_device *usb_dev = usb_data->usb_dev;
	u16 data_len = le16_to_cpu(cmd_hd->len);
	int tx_len, rx_len, ret;

	if (data_len > NCT6694_MAX_DATA_LEN)
		return -EINVAL;

	memcpy(&hdr->cmd_header, cmd_hd, sizeof(*cmd_hd));
	hdr->cmd_header.hctrl = NCT6694_HCTRL_GET;

	/* Send command packet to USB device */
	ret = usb_bulk_msg(usb_dev, usb_sndbulkpipe(usb_dev, NCT6694_BULK_OUT_EP),
			   &hdr->cmd_header, sizeof(*hdr), &tx_len, NCT6694_URB_TIMEOUT);
	if (ret)
		return ret;

	/* Receive response packet from USB device */
	ret = usb_bulk_msg(usb_dev, usb_rcvbulkpipe(usb_dev, NCT6694_BULK_IN_EP),
			   &hdr->response_header, sizeof(*hdr), &rx_len, NCT6694_URB_TIMEOUT);
	if (ret)
		return ret;

	if (rx_len != sizeof(hdr->response_header)) {
		dev_err(ddata->dev, "Expected response length %zu, but got %d\n",
			sizeof(hdr->response_header), rx_len);
		return -EIO;
	}

	/* Receive data packet from USB device */
	ret = usb_bulk_msg(usb_dev, usb_rcvbulkpipe(usb_dev, NCT6694_BULK_IN_EP),
			   usb_data->data_buf, data_len, &rx_len, NCT6694_URB_TIMEOUT);
	if (ret)
		return ret;

	if (rx_len != data_len) {
		dev_err(ddata->dev, "Expected received length %d, but got %d\n",
			data_len, rx_len);
		return -EIO;
	}

	memcpy(rx_buf, usb_data->data_buf, data_len);

	return nct6694_usb_err_handling(ddata, hdr->response_header.sts);
}

/* @rx_buf may be NULL when the reply to the SET command is not needed */
static int nct6694_usb_write_msg(struct nct6694 *ddata,
				 const struct nct6694_cmd_header *cmd_hd,
				 const void *tx_buf, void *rx_buf)
{
	struct nct6694_usb_data *usb_data = to_nct6694_usb_data(ddata);
	union nct6694_usb_hdr *hdr = usb_data->hdr_buf;
	struct usb_device *usb_dev = usb_data->usb_dev;
	u16 data_len = le16_to_cpu(cmd_hd->len);
	int tx_len, rx_len, ret;

	if (data_len > NCT6694_MAX_DATA_LEN)
		return -EINVAL;

	memcpy(&hdr->cmd_header, cmd_hd, sizeof(*cmd_hd));
	hdr->cmd_header.hctrl = NCT6694_HCTRL_SET;
	memcpy(usb_data->data_buf, tx_buf, data_len);

	/* Send command packet to USB device */
	ret = usb_bulk_msg(usb_dev, usb_sndbulkpipe(usb_dev, NCT6694_BULK_OUT_EP),
			   &hdr->cmd_header, sizeof(*hdr), &tx_len, NCT6694_URB_TIMEOUT);
	if (ret)
		return ret;

	/* Send data packet to USB device */
	ret = usb_bulk_msg(usb_dev, usb_sndbulkpipe(usb_dev, NCT6694_BULK_OUT_EP),
			   usb_data->data_buf, data_len, &tx_len, NCT6694_URB_TIMEOUT);
	if (ret)
		return ret;

	/* Receive response packet from USB device */
	ret = usb_bulk_msg(usb_dev, usb_rcvbulkpipe(usb_dev, NCT6694_BULK_IN_EP),
			   &hdr->response_header, sizeof(*hdr), &rx_len, NCT6694_URB_TIMEOUT);
	if (ret)
		return ret;

	if (rx_len != sizeof(hdr->response_header)) {
		dev_err(ddata->dev, "Expected response length %zu, but got %d\n",
			sizeof(hdr->response_header), rx_len);
		return -EIO;
	}

	/* Receive data packet from USB device */
	ret = usb_bulk_msg(usb_dev, usb_rcvbulkpipe(usb_dev, NCT6694_BULK_IN_EP),
			   usb_data->data_buf, data_len, &rx_len, NCT6694_URB_TIMEOUT);
	if (ret)
		return ret;

	if (rx_len != data_len) {
		dev_err(ddata->dev, "Expected transmitted length %d, but got %d\n",
			data_len, rx_len);
		return -EIO;
	}

	if (rx_buf)
		memcpy(rx_buf, usb_data->data_buf, data_len);

	return nct6694_usb_err_handling(ddata, hdr->response_header.sts);
}

static int nct6694_usb_regmap_read(void *context, const void *reg_buf,
				   size_t reg_size, void *val_buf,
				   size_t val_size)
{
	struct nct6694 *ddata = context;
	u32 reg = get_unaligned_be32(reg_buf);
	const struct nct6694_cmd_header cmd_hd = {
		.mod = FIELD_GET(NCT6694_REG_MOD, reg),
		.offset = cpu_to_le16(FIELD_GET(NCT6694_REG_OFFSET, reg)),
		.len = cpu_to_le16(val_size),
	};

	if (val_size > NCT6694_MAX_DATA_LEN)
		return -EINVAL;

	if (FIELD_GET(NCT6694_REG_HCTRL, reg) == NCT6694_HCTRL_SET)
		return nct6694_usb_write_msg(ddata, &cmd_hd, val_buf, val_buf);

	return nct6694_usb_read_msg(ddata, &cmd_hd, val_buf);
}

static int nct6694_usb_regmap_write(void *context, const void *data,
				    size_t count)
{
	struct nct6694 *ddata = context;
	u32 reg = get_unaligned_be32(data);
	const struct nct6694_cmd_header cmd_hd = {
		.mod = FIELD_GET(NCT6694_REG_MOD, reg),
		.offset = cpu_to_le16(FIELD_GET(NCT6694_REG_OFFSET, reg)),
		.len = cpu_to_le16(count - sizeof(reg)),
	};

	if (count - sizeof(reg) > NCT6694_MAX_DATA_LEN)
		return -EINVAL;

	return nct6694_usb_write_msg(ddata, &cmd_hd, data + sizeof(reg), NULL);
}

static const struct regmap_bus nct6694_usb_regmap_bus = {
	.read = nct6694_usb_regmap_read,
	.write = nct6694_usb_regmap_write,
};

static const struct regmap_config nct6694_usb_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
};

static void nct6694_usb_int_callback(struct urb *urb)
{
	struct nct6694 *ddata = urb->context;
	__le32 *status_le = urb->transfer_buffer;
	u32 int_status;
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

	if (urb->actual_length < sizeof(*status_le))
		goto resubmit;

	int_status = le32_to_cpu(*status_le);

	while (int_status) {
		int irq = __ffs(int_status);

		generic_handle_domain_irq_safe(ddata->domain, irq);
		int_status &= ~BIT(irq);
	}

resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret)
		dev_warn(ddata->dev, "Failed to resubmit urb, status %pe",  ERR_PTR(ret));
}

static int nct6694_usb_probe(struct usb_interface *iface,
			     const struct usb_device_id *id)
{
	static const u8 bulk_ep_addr[] = {
		USB_DIR_IN | NCT6694_BULK_IN_EP,
		USB_DIR_OUT | NCT6694_BULK_OUT_EP,
		0
	};
	static const u8 int_ep_addr[] = {
		USB_DIR_IN | NCT6694_INT_IN_EP,
		0
	};
	struct usb_device *usb_dev = interface_to_usbdev(iface);
	struct nct6694_usb_data *usb_data;
	struct device *dev = &iface->dev;
	struct nct6694 *ddata;
	unsigned int int_pipe;
	int ret;

	if (!usb_check_bulk_endpoints(iface, bulk_ep_addr) ||
	    !usb_check_int_endpoints(iface, int_ep_addr))
		return -ENODEV;

	usb_data = devm_kzalloc(dev, sizeof(*usb_data), GFP_KERNEL);
	if (!usb_data)
		return -ENOMEM;

	usb_data->hdr_buf = devm_kzalloc(dev, sizeof(*usb_data->hdr_buf), GFP_KERNEL);
	if (!usb_data->hdr_buf)
		return -ENOMEM;

	usb_data->data_buf = devm_kzalloc(dev, NCT6694_MAX_DATA_LEN, GFP_KERNEL);
	if (!usb_data->data_buf)
		return -ENOMEM;

	usb_data->int_buf = devm_kzalloc(dev, sizeof(*usb_data->int_buf), GFP_KERNEL);
	if (!usb_data->int_buf)
		return -ENOMEM;

	usb_data->int_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!usb_data->int_urb)
		return -ENOMEM;

	ddata = &usb_data->core;
	ddata->dev = dev;
	usb_data->usb_dev = usb_dev;

	ddata->regmap = devm_regmap_init(dev, &nct6694_usb_regmap_bus, ddata,
					 &nct6694_usb_regmap_config);
	if (IS_ERR(ddata->regmap)) {
		ret = PTR_ERR(ddata->regmap);
		goto err_free_urb;
	}

	int_pipe = usb_rcvintpipe(usb_dev, NCT6694_INT_IN_EP);
	usb_fill_int_urb(usb_data->int_urb, usb_dev, int_pipe,
			 usb_data->int_buf, sizeof(*usb_data->int_buf),
			 nct6694_usb_int_callback, ddata,
			 usb_pipe_endpoint(usb_dev, int_pipe)->desc.bInterval);

	usb_set_intfdata(iface, ddata);

	ret = nct6694_device_init(dev);
	if (ret)
		goto err_free_urb;

	ret = usb_submit_urb(usb_data->int_urb, GFP_KERNEL);
	if (ret)
		goto err_device_exit;

	ret = mfd_add_hotplug_devices(dev, nct6694_usb_devs, ARRAY_SIZE(nct6694_usb_devs));
	if (ret)
		goto err_kill_urb;

	return 0;

err_kill_urb:
	usb_kill_urb(usb_data->int_urb);
err_device_exit:
	nct6694_device_exit(dev);
err_free_urb:
	usb_free_urb(usb_data->int_urb);
	return ret;
}

static void nct6694_usb_disconnect(struct usb_interface *iface)
{
	struct nct6694 *ddata = usb_get_intfdata(iface);
	struct nct6694_usb_data *usb_data = to_nct6694_usb_data(ddata);

	mfd_remove_devices(ddata->dev);
	usb_kill_urb(usb_data->int_urb);
	nct6694_device_exit(ddata->dev);
	usb_free_urb(usb_data->int_urb);
}

static const struct usb_device_id nct6694_usb_ids[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(NCT6694_VENDOR_ID, NCT6694_PRODUCT_ID, 0xFF, 0x00, 0x00) },
	{ }
};
MODULE_DEVICE_TABLE(usb, nct6694_usb_ids);

static struct usb_driver nct6694_usb_driver = {
	.name		= "nct6694-usb",
	.id_table	= nct6694_usb_ids,
	.probe		= nct6694_usb_probe,
	.disconnect	= nct6694_usb_disconnect,
};
module_usb_driver(nct6694_usb_driver);

MODULE_DESCRIPTION("Nuvoton NCT6694 USB transport driver");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
