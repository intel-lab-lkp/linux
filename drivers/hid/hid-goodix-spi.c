// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Goodix GT7986U SPI Driver Code for HID.
 *
 * Copyright (C) 2024 Godix, Inc.
 */
#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/spi/spi.h>

#define GOODIX_DEV_CONFIRM_ADDR		0x10000
#define GOODIX_HID_DESC_ADDR		0x1058C
#define GOODIX_HID_REPORT_DESC_ADDR	0x105AA
#define GOODIX_HID_SIGN_ADDR		0x10D32

#define GOODIX_HID_GET_REPORT_CMD	0x02
#define GOODIX_HID_SET_REPORT_CMD	0x03

#define GOODIX_HID_MAX_INBUF_SIZE	128
#define GOODIX_HID_ACK_READY_FLAG	0x01
#define GOODIX_HID_REPORT_READY_FLAG	0x80

#define GOODIX_DEV_CONFIRM_VAL		0xAA

#define GOODIX_SPI_WRITE_FLAG		0xF0
#define GOODIX_SPI_READ_FLAG		0xF1
#define GOODIX_SPI_TRANS_PREFIX_LEN	1
#define GOODIX_REGISTER_WIDTH		4
#define GOODIX_SPI_READ_DUMMY_LEN	3
#define GOODIX_SPI_READ_PREFIX_LEN	(GOODIX_SPI_TRANS_PREFIX_LEN + \
					 GOODIX_REGISTER_WIDTH + \
					 GOODIX_SPI_READ_DUMMY_LEN)
#define GOODIX_SPI_WRITE_PREFIX_LEN	(GOODIX_SPI_TRANS_PREFIX_LEN + \
					 GOODIX_REGISTER_WIDTH)

#define GOODIX_CHECKSUM_SIZE		sizeof(u16)
#define GOODIX_NORMAL_RESET_DELAY_MS	150

struct goodix_hid_report_header {
	u8 flag;
	__le16 size;
} __packed;
#define GOODIX_HID_ACK_HEADER_SIZE	sizeof(struct goodix_hid_report_header)

struct goodix_hid_report_package {
	__le16 size;
	u8 data[];
};

#define GOODIX_HID_PKG_LEN_SIZE		sizeof(u16)
#define GOODIX_HID_COOR_DATA_LEN	82
#define GOODIX_HID_COOR_PKG_LEN		(GOODIX_HID_PKG_LEN_SIZE + \
					 GOODIX_HID_COOR_DATA_LEN)

#define GOODIX_HID_MAX_PKG_SIZE		(GOODIX_HID_COOR_PKG_LEN + SZ_4K)

struct goodix_hid_report_event {
	struct goodix_hid_report_header hdr;
	u8 data[GOODIX_HID_COOR_PKG_LEN];
} __packed;

struct goodix_hid_desc {
	__le16 desc_length;
	__le16 bcd_version;
	__le16 report_desc_lenght;
	__le16 report_desc_register;
	__le16 input_register;
	__le16 max_input_length;
	__le16 output_register;
	__le16 max_output_length;
	__le16 cmd_register;
	__le16 data_register;
	__le16 vendor_id;
	__le16 product_id;
	__le16 version_id;
	__le32 reserved;
} __packed;

struct goodix_ts_data {
	struct device *dev;
	struct spi_device *spi;
	struct hid_device *hid;
	struct goodix_hid_desc hid_desc;

	struct gpio_desc *reset_gpio;
	u32 hid_report_addr;

	/* lock for hid raw request operation */
	struct mutex hid_request_lock;
	/* lock for reg read write operations */
	struct mutex reg_rw_lock;
	u32 spi_xfer_max_sz;
	/* buffer used to store hid report event */
	u8 event_buf[SZ_4K] ____cacheline_aligned;
	/* buffer used to do spi data transfer */
	u8 xfer_buf[GOODIX_HID_MAX_PKG_SIZE] ____cacheline_aligned;
};

static int goodix_spi_read(struct goodix_ts_data *ts, u32 addr,
			   u8 *data, unsigned int len)
{
	struct spi_device *spi = to_spi_device(&ts->spi->dev);
	struct spi_transfer xfers;
	struct spi_message spi_msg;
	int error;

	if (GOODIX_SPI_READ_PREFIX_LEN + len > ts->spi_xfer_max_sz) {
		dev_err(ts->dev, "read data len exceed limit %d",
			ts->spi_xfer_max_sz - GOODIX_SPI_READ_PREFIX_LEN);
		return -EINVAL;
	}

	mutex_lock(&ts->reg_rw_lock);
	/* buffer format: 0xF1 + addr(4bytes) + dummy(3bytes) + data */
	ts->xfer_buf[0] = GOODIX_SPI_READ_FLAG;
	put_unaligned_be32(addr, ts->xfer_buf + GOODIX_SPI_TRANS_PREFIX_LEN);

	spi_message_init(&spi_msg);
	memset(&xfers, 0, sizeof(xfers));
	xfers.tx_buf = ts->xfer_buf;
	xfers.rx_buf = ts->xfer_buf;
	xfers.len = GOODIX_SPI_READ_PREFIX_LEN + len;
	spi_message_add_tail(&xfers, &spi_msg);

	error = spi_sync(spi, &spi_msg);
	if (error)
		dev_err(ts->dev, "spi transfer error:%d", error);
	else
		memcpy(data, ts->xfer_buf + GOODIX_SPI_READ_PREFIX_LEN, len);

	mutex_unlock(&ts->reg_rw_lock);
	return error;
}

static int goodix_spi_write(struct goodix_ts_data *ts, u32 addr,
			    u8 *data, unsigned int len)
{
	struct spi_device *spi = to_spi_device(&ts->spi->dev);
	struct spi_transfer xfers;
	struct spi_message spi_msg;
	int error;

	if (GOODIX_SPI_WRITE_PREFIX_LEN + len > ts->spi_xfer_max_sz) {
		dev_err(ts->dev, "write data len exceed limit %d",
			ts->spi_xfer_max_sz - GOODIX_SPI_WRITE_PREFIX_LEN);
		return -EINVAL;
	}

	mutex_lock(&ts->reg_rw_lock);
	/* buffer format: 0xF0 + addr(4bytes) + data */
	ts->xfer_buf[0] = GOODIX_SPI_WRITE_FLAG;
	put_unaligned_be32(addr, ts->xfer_buf + GOODIX_SPI_TRANS_PREFIX_LEN);
	memcpy(ts->xfer_buf + GOODIX_SPI_WRITE_PREFIX_LEN, data, len);

	spi_message_init(&spi_msg);
	memset(&xfers, 0, sizeof(xfers));
	xfers.tx_buf = ts->xfer_buf;
	xfers.len = GOODIX_SPI_WRITE_PREFIX_LEN + len;
	spi_message_add_tail(&xfers, &spi_msg);

	error = spi_sync(spi, &spi_msg);
	if (error)
		dev_err(ts->dev, "spi transfer error:%d", error);

	mutex_unlock(&ts->reg_rw_lock);
	return error;
}

static int goodix_dev_confirm(struct goodix_ts_data *ts)
{
	u8 tx_buf[8], rx_buf[8];
	int retry = 3;
	int error;

	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	usleep_range(4000, 4100);

	memset(tx_buf, GOODIX_DEV_CONFIRM_VAL, sizeof(tx_buf));
	while (retry--) {
		error = goodix_spi_write(ts, GOODIX_DEV_CONFIRM_ADDR,
					 tx_buf, sizeof(tx_buf));
		if (error)
			return error;

		error = goodix_spi_read(ts, GOODIX_DEV_CONFIRM_ADDR,
					rx_buf, sizeof(rx_buf));
		if (error)
			return error;

		if (!memcmp(tx_buf, rx_buf, sizeof(tx_buf)))
			return 0;

		usleep_range(5000, 5100);
	}

	dev_err(ts->dev, "device confirm failed, rx_buf:%*ph", 8, rx_buf);
	return -EINVAL;
}

/**
 * goodix_hid_parse() - hid-core .parse() callback
 * @hid: hid device instance
 *
 * This function gets called during call to hid_add_device
 *
 * Return: 0 on success and non zero on error
 */
static int goodix_hid_parse(struct hid_device *hid)
{
	struct goodix_ts_data *ts = hid->driver_data;
	u8 *rdesc __free(kfree);
	u16 rsize;
	int error;

	rsize = le16_to_cpu(ts->hid_desc.report_desc_lenght);
	if (!rsize || rsize > HID_MAX_DESCRIPTOR_SIZE) {
		dev_err(ts->dev, "invalid report desc size %d", rsize);
		return -EINVAL;
	}

	rdesc = kzalloc(rsize, GFP_KERNEL);
	if (!rdesc)
		return -ENOMEM;

	error = goodix_spi_read(ts, GOODIX_HID_REPORT_DESC_ADDR, rdesc, rsize);
	if (error) {
		dev_err(ts->dev, "failed get report desc, %d", error);
		return error;
	}

	error = hid_parse_report(hid, rdesc, rsize);
	if (error)
		dev_err(ts->dev, "failed parse report, %d", error);

	return error;
}

/* Empty callbacks with success return code */
static int goodix_hid_start(struct hid_device *hid)
{
	return 0;
}

static void goodix_hid_stop(struct hid_device *hid)
{
}

static int goodix_hid_open(struct hid_device *hid)
{
	return 0;
}

static void goodix_hid_close(struct hid_device *hid)
{
}

/* Return date length of response data */
static int goodix_hid_check_ack_status(struct goodix_ts_data *ts)
{
	struct goodix_hid_report_header hdr;
	int retry = 20;
	int error;

	while (retry--) {
		/*
		 * 3 bytes of hid request response data
		 * - byte 0:    Ack flag, value of 1 for data ready
		 * - bytes 1-2: Response data length
		 */
		error = goodix_spi_read(ts, ts->hid_report_addr,
					(u8 *)&hdr, sizeof(hdr));
		if (!error && (hdr.flag & GOODIX_HID_ACK_READY_FLAG))
			return le16_to_cpu(hdr.size);

		/* Wait 10ms for another try */
		usleep_range(10000, 11000);
	}

	return -EINVAL;
}

/**
 * goodix_hid_get_raw_report() - Process hidraw GET REPORT operation
 * @hid: hid device instance
 * @reportnum: Report ID
 * @buf: Buffer for store the reprot date
 * @len: Length fo reprot data
 * @report_type: Report type
 *
 * The function for hid_ll_driver.get_raw_report to handle the HIDRAW ioctl
 * get report request. The transmitted data follows the standard i2c-hid
 * protocol with a specified header.
 *
 * Return: The length of the data in the buf on success, negative error code
 */
static int goodix_hid_get_raw_report(struct hid_device *hid,
				     unsigned char reportnum,
				     __u8 *buf, size_t len,
				     unsigned char report_type)
{
	struct goodix_ts_data *ts = hid->driver_data;
	u16 data_register = le16_to_cpu(ts->hid_desc.data_register);
	u16 cmd_register = le16_to_cpu(ts->hid_desc.cmd_register);
	u8 tmp_buf[GOODIX_HID_MAX_INBUF_SIZE];
	int tx_len = 0, args_len = 0;
	int response_data_len;
	u8 args[3];
	int error;

	if (report_type == HID_OUTPUT_REPORT)
		return -EINVAL;

	if (reportnum == 3) {
		/* Get win8 signature data */
		error = goodix_spi_read(ts, GOODIX_HID_SIGN_ADDR, buf, len);
		if (error) {
			dev_err(ts->dev, "failed get win8 sign:%d", error);
			return -EINVAL;
		}
		return len;
	}

	if (reportnum >= 0x0F)
		args[args_len++] = reportnum;

	put_unaligned_le16(data_register, args + args_len);
	args_len += sizeof(data_register);

	/* Clean 3 bytes of hid ack header data */
	memset(tmp_buf, 0, GOODIX_HID_ACK_HEADER_SIZE);
	tx_len += GOODIX_HID_ACK_HEADER_SIZE;

	put_unaligned_le16(cmd_register, tmp_buf + tx_len);
	tx_len += sizeof(cmd_register);

	tmp_buf[tx_len] = (report_type == HID_FEATURE_REPORT ? 0x03 : 0x01) << 4;
	tmp_buf[tx_len] |=  reportnum >= 0x0F ? 0x0F : reportnum;
	tx_len++;

	tmp_buf[tx_len++] = GOODIX_HID_GET_REPORT_CMD;

	memcpy(tmp_buf + tx_len, args, args_len);
	tx_len += args_len;

	/* Step1: write report request info */
	error = goodix_spi_write(ts, ts->hid_report_addr, tmp_buf, tx_len);
	if (error) {
		dev_err(ts->dev, "failed send read feature cmd, %d", error);
		return error;
	}

	/* No need read response data */
	if (!len)
		return 0;

	/* Step2: check response data status */
	response_data_len = goodix_hid_check_ack_status(ts);
	if (response_data_len <= GOODIX_HID_PKG_LEN_SIZE)
		return -EINVAL;

	len = min(len, response_data_len - GOODIX_HID_PKG_LEN_SIZE);
	/* Step3: read response data(skip 2bytes of hid pkg length) */
	error = goodix_spi_read(ts, ts->hid_report_addr +
				GOODIX_HID_ACK_HEADER_SIZE +
				GOODIX_HID_PKG_LEN_SIZE, buf, len);
	if (error) {
		dev_err(ts->dev, "failed read hid response data, %d", error);
		return error;
	}

	if (buf[0] != reportnum) {
		dev_err(ts->dev, "incorrect reprot (%d vs %d expected)",
			buf[0], reportnum);
		return -EINVAL;
	}
	return len;
}

/**
 * goodix_hid_set_raw_report() - process hidraw SET REPORT operation
 * @hid: HID device
 * @reportnum: Report ID
 * @buf: Buffer for communication
 * @len: Length of data in the buffer
 * @report_type: Report type
 *
 * The function for hid_ll_driver.get_raw_report to handle the HIDRAW ioctl
 * set report request. The transmitted data follows the standard i2c-hid
 * protocol with a specified header.
 *
 * Return: The length of the data sent, negative error code on failure
 */
static int goodix_hid_set_raw_report(struct hid_device *hid,
				     unsigned char reportnum,
				     __u8 *buf, size_t len,
				     unsigned char report_type)
{
	struct goodix_ts_data *ts = hid->driver_data;
	u16 data_register = le16_to_cpu(ts->hid_desc.data_register);
	u16 cmd_register = le16_to_cpu(ts->hid_desc.cmd_register);
	int tx_len = 0, args_len = 0;
	u8 tmp_buf[GOODIX_HID_MAX_INBUF_SIZE];
	u8 args[5];
	int error;

	if (reportnum >= 0x0F) {
		args[args_len++] = reportnum;
		reportnum = 0x0F;
	}

	put_unaligned_le16(data_register, args + args_len);
	args_len += sizeof(data_register);

	put_unaligned_le16(GOODIX_HID_PKG_LEN_SIZE + len, args + args_len);
	args_len += GOODIX_HID_PKG_LEN_SIZE;

	/* Clean 3 bytes of hid ack header data */
	memset(tmp_buf, 0, GOODIX_HID_ACK_HEADER_SIZE);
	tx_len += GOODIX_HID_ACK_HEADER_SIZE;

	put_unaligned_le16(cmd_register, tmp_buf + tx_len);
	tx_len += sizeof(cmd_register);

	tmp_buf[tx_len++] = ((report_type == HID_FEATURE_REPORT ? 0x03 : 0x02) << 4) | reportnum;
	tmp_buf[tx_len++] = GOODIX_HID_SET_REPORT_CMD;

	memcpy(tmp_buf + tx_len, args, args_len);
	tx_len += args_len;

	memcpy(tmp_buf + tx_len, buf, len);
	tx_len += len;

	error = goodix_spi_write(ts, ts->hid_report_addr, tmp_buf, tx_len);
	if (error) {
		dev_err(ts->dev, "failed send report %*ph", tx_len, tmp_buf);
		return error;
	}
	return len;
}

static int goodix_hid_raw_request(struct hid_device *hid,
				  unsigned char reportnum,
				  __u8 *buf, size_t len,
				  unsigned char rtype, int reqtype)
{
	struct goodix_ts_data *ts = hid->driver_data;
	int error = -EINVAL;

	mutex_lock(&ts->hid_request_lock);
	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		error = goodix_hid_get_raw_report(hid, reportnum, buf,
						  len, rtype);
		break;
	case HID_REQ_SET_REPORT:
		if (buf[0] == reportnum)
			error = goodix_hid_set_raw_report(hid, reportnum,
							  buf, len, rtype);
		break;
	default:
		break;
	}
	mutex_unlock(&ts->hid_request_lock);

	return error;
}

static struct hid_ll_driver goodix_hid_ll_driver = {
	.parse = goodix_hid_parse,
	.start = goodix_hid_start,
	.stop = goodix_hid_stop,
	.open = goodix_hid_open,
	.close = goodix_hid_close,
	.raw_request = goodix_hid_raw_request
};

static irqreturn_t goodix_hid_irq(int irq, void *data)
{
	struct goodix_ts_data *ts = data;
	struct goodix_hid_report_event event;
	struct goodix_hid_report_package *pkg;
	u16 report_size;
	int error;

	/*
	 * First, read buffer with space for header and coordinate package:
	 * - event header = 3 bytes
	 * - coordinate event = GOODIX_HID_COOR_PKG_LEN bytes
	 *
	 * If the data size info in the event header exceeds
	 * GOODIX_HID_COOR_PKG_LEN, it means that there are other packages
	 * besides the coordinate package.
	 */
	error = goodix_spi_read(ts, ts->hid_report_addr, (u8 *)&event,
				sizeof(event));
	if (error) {
		dev_err(ts->dev, "failed get coordinate data, %d", error);
		return IRQ_HANDLED;
	}

	/* Check coordinate data valid falg */
	if (event.hdr.flag != GOODIX_HID_REPORT_READY_FLAG) {
		dev_err(ts->dev, "invalid event flag 0x%x", event.hdr.flag);
		return IRQ_HANDLED;
	}

	pkg = (struct goodix_hid_report_package *)event.data;
	hid_input_report(ts->hid, HID_INPUT_REPORT, pkg->data,
			 le16_to_cpu(pkg->size) - GOODIX_HID_PKG_LEN_SIZE, 1);

	report_size = le16_to_cpu(event.hdr.size);
	/* Check if there are other packages */
	if (report_size <= GOODIX_HID_COOR_PKG_LEN)
		return IRQ_HANDLED;

	if (report_size - GOODIX_HID_COOR_PKG_LEN > sizeof(ts->event_buf)) {
		dev_err(ts->dev, "invalid package size, %d", report_size);
		return IRQ_HANDLED;
	}

	/* Read the package behind the coordinate data */
	error = goodix_spi_read(ts, ts->hid_report_addr + sizeof(event),
				ts->event_buf,
				report_size - GOODIX_HID_COOR_PKG_LEN);
	if (error) {
		dev_err(ts->dev, "failed read data, %d", error);
		return IRQ_HANDLED;
	}

	pkg = (struct goodix_hid_report_package *)ts->event_buf;
	hid_input_report(ts->hid, HID_INPUT_REPORT, pkg->data,
			 le16_to_cpu(pkg->size) - GOODIX_HID_PKG_LEN_SIZE, 1);

	return IRQ_HANDLED;
}

static int goodix_hid_init(struct goodix_ts_data *ts)
{
	struct hid_device *hid;
	int error;

	/* Get hid descriptor */
	error = goodix_spi_read(ts, GOODIX_HID_DESC_ADDR, (u8 *)&ts->hid_desc,
				sizeof(ts->hid_desc));
	if (error) {
		dev_err(ts->dev, "failed get hid desc, %d", error);
		return error;
	}

	hid = hid_allocate_device();
	if (IS_ERR(hid))
		return PTR_ERR(hid);

	hid->driver_data = ts;
	hid->ll_driver = &goodix_hid_ll_driver;
	hid->bus = BUS_SPI;
	hid->dev.parent = &ts->spi->dev;

	hid->version = le16_to_cpu(ts->hid_desc.bcd_version);
	hid->vendor = le16_to_cpu(ts->hid_desc.vendor_id);
	hid->product = le16_to_cpu(ts->hid_desc.product_id);
	snprintf(hid->name, sizeof(hid->name), "%s %04X:%04X", "hid-gdix",
		 hid->vendor, hid->product);

	error = hid_add_device(hid);
	if (error) {
		dev_err(ts->dev, "failed add hid device, %d", error);
		hid_destroy_device(hid);
		return error;
	}

	ts->hid = hid;
	return 0;
}

static int goodix_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct goodix_ts_data *ts;
	int error;

	/* init spi_device */
	spi->mode            = SPI_MODE_0;
	spi->bits_per_word   = 8;
	error = spi_setup(spi);
	if (error)
		return error;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	mutex_init(&ts->hid_request_lock);
	mutex_init(&ts->reg_rw_lock);
	spi_set_drvdata(spi, ts);
	if (spi->controller->max_transfer_size)
		ts->spi_xfer_max_sz = spi->controller->max_transfer_size(spi);
	else
		ts->spi_xfer_max_sz = GOODIX_HID_MAX_PKG_SIZE;

	ts->spi_xfer_max_sz = min(GOODIX_HID_MAX_PKG_SIZE, ts->spi_xfer_max_sz);
	ts->spi = spi;
	ts->dev = dev;
	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio),
				     "failed to request reset gpio\n");

	error = device_property_read_u32(dev, "hid-report-addr",
					 &ts->hid_report_addr);
	if (error)
		return dev_err_probe(dev, error,
				     "failed get hid report addr\n");

	error = goodix_dev_confirm(ts);
	if (error)
		return error;

	/* Waits 150ms for firmware to fully boot */
	msleep(GOODIX_NORMAL_RESET_DELAY_MS);

	error = goodix_hid_init(ts);
	if (error) {
		dev_err(dev, "failed init hid device");
		return error;
	}

	error = devm_request_threaded_irq(&ts->spi->dev, ts->spi->irq,
					  NULL, goodix_hid_irq, IRQF_ONESHOT,
					  "goodix_spi_hid", ts);
	if (error < 0) {
		dev_err(ts->dev, "could not register interrupt, irq = %d, %d",
			ts->spi->irq, error);
		goto err_destroy_hid;
	}

	return 0;

err_destroy_hid:
	hid_destroy_device(ts->hid);
	return error;
}

static void goodix_spi_remove(struct spi_device *spi)
{
	struct goodix_ts_data *ts = spi_get_drvdata(spi);

	disable_irq(spi->irq);
	hid_destroy_device(ts->hid);
}

static void goodix_spi_shutdown(struct spi_device *spi)
{
	struct goodix_ts_data *ts = spi_get_drvdata(spi);

	disable_irq(spi->irq);
	hid_destroy_device(ts->hid);
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id goodix_spi_acpi_match[] = {
	{ "GXTS7986" },
	{ },
};
MODULE_DEVICE_TABLE(acpi, goodix_spi_acpi_match);
#endif

static struct spi_driver goodix_spi_driver = {
	.driver = {
		.name = "goodix-spi-hid",
		.acpi_match_table = ACPI_PTR(goodix_spi_acpi_match),
	},
	.probe =	goodix_spi_probe,
	.remove =	goodix_spi_remove,
	.shutdown =	goodix_spi_shutdown,
};
module_spi_driver(goodix_spi_driver);

MODULE_DESCRIPTION("Goodix SPI driver for HID touchscreen");
MODULE_AUTHOR("Goodix, Inc.");
MODULE_LICENSE("GPL");
