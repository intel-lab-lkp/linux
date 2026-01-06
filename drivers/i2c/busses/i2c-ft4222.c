// SPDX-License-Identifier: GPL-2.0
/*
 * FTDI FT4222H USB-to-I2C bridge
 */

#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/usb.h>

#define FT4222_FLAG_START	0x2
#define FT4222_FLAG_RESTART	0x3
#define FT4222_FLAG_STOP	0x4
#define FT4222_FLAG_NONE	0x80

/**
 * Tx header:
 * - I2C device address + R/W bit (1 byte)
 * - flags (1 byte)
 * - message length (2 bytes)
 */
#define FT4222_TX_HDRLEN	4

#define FT4222_RX_HDRLEN	2

#define FT4222_BULK_MAXLEN	512
#define FT4222_IO_TIMEOUT	5000

/* Status flags */
#define FT4222_I2C_CTRL_BUSY	(1 << 0)
#define FT4222_I2C_ERROR	(1 << 1)
#define FT4222_I2C_ADDR_NACK	(1 << 2)
#define FT4222_I2C_DATA_NACK	(1 << 3)
#define FT4222_I2C_ARB_LOST	(1 << 4)
#define FT4222_I2C_CTRL_IDLE	(1 << 5)
#define FT4222_I2C_BUS_BUSY	(1 << 6)

enum ft4222_conf_mode {
	ft4222_conf0,
	ft4222_conf12,
	ft4222_conf3,
};

enum ft4222_sys_clk {
	ft4222_sys_clk_60 = 0,
	ft4222_sys_clk_24,
	ft4222_sys_clk_48,
	ft4222_sys_clk_80,
};

struct ft4222_i2c {
	struct i2c_adapter adapter;
	struct usb_device *udev;
	u8 ubuf[FT4222_BULK_MAXLEN];
	unsigned int sys_clk;	/* system clock frequency */
	unsigned int freq;	/* I2C bus frequency */
};

static int ft4222_cmd_set(struct ft4222_i2c *ftdi, u8 cmd, u8 val)
{
	struct usb_device *udev = ftdi->udev;

	return usb_control_msg(udev, usb_sndctrlpipe(udev, 0), 0x21, 0x40,
			       cmd | (((u16)val) << 8), 0x0001, NULL, 0,
			       FT4222_IO_TIMEOUT);
}

static int ft4222_cmd_get(struct ft4222_i2c *ftdi, u16 cmd, u8 *val)
{
	struct usb_device *udev = ftdi->udev;
	int ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0), 0x20, 0xC0,
				  cmd, 0x0001, ftdi->ubuf, sizeof(*val),
				  FT4222_IO_TIMEOUT);

	if (ret == sizeof(*val))
		*val = ftdi->ubuf[0];
	else if (ret >= 0)	/* unexpected number of bytes transferred */
		ret = -EIO;
	return ret;
}

static int ft4222_i2c_reset(struct ft4222_i2c *ftdi)
{
	return ft4222_cmd_set(ftdi, 0x51, 1);
}

static int ft4222_i2c_get_status(struct ft4222_i2c *ftdi)
{
	u8 status;
	int retry;
	const int max_retries = 512;

	for (retry = 0; retry < max_retries; retry++) {
		int ret = ft4222_cmd_get(ftdi, 0xf5b4, &status);

		if (ret < 0)
			return ret;
		if (!(status & FT4222_I2C_CTRL_BUSY))
			break;
	}
	dev_dbg(&ftdi->adapter.dev, "status 0x%02x (%d retries)", status,
		retry);
	if (retry == max_retries) {
		ft4222_i2c_reset(ftdi);
		return -ETIMEDOUT;
	}
	if (!(status & FT4222_I2C_ERROR))
		return 0;
	if (status & FT4222_I2C_ADDR_NACK)
		return -ENXIO;
	else if (status & FT4222_I2C_DATA_NACK)
		return -EIO;
	else
		return -EBUSY;
}

static int ft4222_i2c_write(struct ft4222_i2c *ftdi, u8 flags, u8 slave_addr,
			    u8 *data, int len)
{
	struct usb_device *udev = ftdi->udev;
	unsigned int pipe = usb_sndbulkpipe(udev, 2);
	u8 *buf = ftdi->ubuf;
	int written = 0;

	dev_dbg(&ftdi->adapter.dev, "write to 0x%02x, flags 0x%02x, len %d",
		slave_addr, flags, len);
	buf[0] = slave_addr << 1;
	buf[2] = buf[3] = 0;
	do {
		int pkt_len = min(FT4222_TX_HDRLEN + len - written,
				  FT4222_BULK_MAXLEN);
		bool first_pkt = (written == 0);
		bool last_pkt = (written + pkt_len == FT4222_TX_HDRLEN + len);
		int ret, actual_len;

		buf[1] = 0;
		if (first_pkt)
			buf[1] |= flags & FT4222_FLAG_RESTART;
		if (last_pkt)
			buf[1] |= flags & FT4222_FLAG_STOP;
		if (buf[1] == 0)
			buf[1] = FT4222_FLAG_NONE;
		memcpy(buf + FT4222_TX_HDRLEN, data + written,
		       pkt_len - FT4222_TX_HDRLEN);
		ret = usb_bulk_msg(udev, pipe, buf, pkt_len, &actual_len,
				   FT4222_IO_TIMEOUT);
		if (ret < 0)
			return ret;
		if (actual_len < pkt_len)
			return -EIO;
		ret = ft4222_i2c_get_status(ftdi);
		if (ret < 0)
			return ret;
		written += pkt_len - FT4222_TX_HDRLEN;
	} while (written < len);
	return 0;
}

static int ft4222_i2c_read(struct ft4222_i2c *ftdi, u8 flags, u8 slave_addr,
			   u8 *data, int len)
{
	struct usb_device *udev = ftdi->udev;
	unsigned int pipe = usb_rcvbulkpipe(udev, 1);
	u8 *buf = ftdi->ubuf;
	int ret;
	int actual_len;
	int read = 0;

	dev_dbg(&ftdi->adapter.dev, "read from 0x%02x, flags 0x%02x, len %d",
		slave_addr, flags, len);
	buf[0] = (slave_addr << 1) | 1;
	buf[1] = flags;
	buf[2] = len >> 8;
	buf[3] = len;
	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, 2), buf,
			   FT4222_TX_HDRLEN, &actual_len, FT4222_IO_TIMEOUT);
	if (ret < 0)
		return ret;
	if (actual_len != FT4222_TX_HDRLEN)
		return -EIO;
	do {
		int pkt_len = min(FT4222_RX_HDRLEN + len - read,
				  FT4222_BULK_MAXLEN);

		ret = usb_bulk_msg(udev, pipe, buf, pkt_len, &actual_len,
				   FT4222_IO_TIMEOUT);
		if (ret < 0)
			return ret;
		if (actual_len < FT4222_RX_HDRLEN)
			return -EIO;
		actual_len -= FT4222_RX_HDRLEN;
		memcpy(data + read, buf + FT4222_RX_HDRLEN, actual_len);
		read += actual_len;
	} while (read < len);
	return ft4222_i2c_get_status(ftdi);
}

static int ft4222_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msg,
			   int num)
{
	struct ft4222_i2c *ftdi = container_of(adapter, struct ft4222_i2c,
					       adapter);
	int i;
	int ret;

	dev_dbg(&adapter->dev, "transfer with %d message(s)", num);
	for (i = 0; i < num; ++i) {
		const u8 addr = msg[i].addr;
		u8 *buf = msg[i].buf;
		const u16 len = msg[i].len;
		u8 flags;

		flags = ((i == 0) ? FT4222_FLAG_START : FT4222_FLAG_RESTART);
		if (i == num - 1)
			flags |= FT4222_FLAG_STOP;
		if (msg[i].flags & I2C_M_RD)
			ret = ft4222_i2c_read(ftdi, flags, addr, buf, len);
		else
			ret = ft4222_i2c_write(ftdi, flags, addr, buf, len);
		if (ret < 0)
			goto err;
	}
	return num;
err:
	ft4222_i2c_reset(ftdi);
	return ret;
}

static u32 ft4222_i2c_func(struct i2c_adapter *adapter)
{
	/**
	 * The device seems to be unable to perform I2C transactions with 0 data
	 * length, hence no support for SMBus quick command.
	 */
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL_ALL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm ft4222_i2c_algo = {
	.master_xfer = ft4222_i2c_xfer,
	.functionality = ft4222_i2c_func,
};

static int ft4222_i2c_setup(struct ft4222_i2c *ftdi, unsigned int freq)
{
	bool hi_freq = (freq > I2C_MAX_FAST_MODE_FREQ);
	const int m = hi_freq ? 6 : 8;
	u8 n;
	int ret;

	if ((freq < I2C_MAX_STANDARD_MODE_FREQ) ||
	    (freq > I2C_MAX_HIGH_SPEED_MODE_FREQ))
		return -EINVAL;
	n = DIV_ROUND_UP(ftdi->sys_clk, freq * m) - 1;
	if (hi_freq)
		n |= 0xc0;
	ret = ft4222_cmd_set(ftdi, 0x52, n);
	if (ret < 0)
		return ret;
	ret = ft4222_cmd_set(ftdi, 0x05, 1);
	if (ret < 0)
		return ret;
	ftdi->freq = freq;
	return 0;
}

static struct ft4222_i2c *ft4222_i2c_from_dev(struct device *dev)
{
	struct i2c_adapter *adapter = to_i2c_adapter(dev);

	return container_of(adapter, struct ft4222_i2c, adapter);
}

static ssize_t i2c_freq_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct ft4222_i2c *ftdi = ft4222_i2c_from_dev(dev);

	return sysfs_emit(buf, "%u\n", ftdi->freq);
}

static ssize_t i2c_freq_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned int i2c_freq;
	int ret = kstrtouint(buf, 10, &i2c_freq);

	if (!ret) {
		struct ft4222_i2c *ftdi = ft4222_i2c_from_dev(dev);

		ret = ft4222_i2c_setup(ftdi, i2c_freq);
	}
	return ret < 0 ? ret : count;
}

static DEVICE_ATTR_RW(i2c_freq);

static struct attribute *ft4222_attrs[] = {
	&dev_attr_i2c_freq.attr,
	NULL
};

static const struct attribute_group ft4222_attr_group = {
	.attrs = ft4222_attrs,
};

static const struct attribute_group *ft4222_attr_groups[] = {
	&ft4222_attr_group,
	NULL
};

static int ft4222_i2c_init(struct usb_interface *interface)
{
	struct device *dev = &interface->dev;
	struct ft4222_i2c *ftdi = devm_kmalloc(dev, sizeof(*ftdi), GFP_KERNEL);
	int ret;
	u8 sys_clk_enum;
	unsigned int sys_clk;
	struct i2c_adapter *adapter;

	if (!ftdi)
		return -ENOMEM;
	ftdi->udev = interface_to_usbdev(interface);
	ret = ft4222_cmd_get(ftdi, 0x0004, &sys_clk_enum);
	if (ret < 0)
		return ret;
	switch (sys_clk_enum) {
	case ft4222_sys_clk_60:
		sys_clk = 60000000;
		break;
	case ft4222_sys_clk_24:
		sys_clk = 24000000;
		break;
	case ft4222_sys_clk_48:
		sys_clk = 48000000;
		break;
	case ft4222_sys_clk_80:
		sys_clk = 80000000;
		break;
	default:
		dev_err(dev, "unknown system clock setting %d", sys_clk_enum);
		return -EOPNOTSUPP;
	}
	ftdi->sys_clk = sys_clk;
	ret = ft4222_i2c_setup(ftdi, I2C_MAX_FAST_MODE_FREQ);
	if (ret < 0)
		return ret;
	ret = ft4222_i2c_reset(ftdi);
	if (ret < 0)
		return ret;
	adapter = &ftdi->adapter;
	memset(adapter, 0, sizeof(*adapter));
	adapter->owner = THIS_MODULE;
	adapter->algo = &ft4222_i2c_algo;
	adapter->dev.parent = dev;
	adapter->dev.groups = ft4222_attr_groups;
	snprintf(adapter->name, sizeof(adapter->name),
		 "FT4222 USB-to-I2C %03d-%03d", ftdi->udev->bus->busnum,
		 ftdi->udev->devnum);
	ret = devm_i2c_add_adapter(dev, adapter);
	if (ret < 0)
		return ret;
	dev_dbg(&adapter->dev, "system clock frequency %d Hz", sys_clk);
	return 0;
}

static int ft4222_get_conf(struct usb_interface *interface,
			   enum ft4222_conf_mode *conf_mode)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	u16 dev_type = udev->descriptor.bcdDevice;

	switch (dev_type >> 8) {
	case 0x17:
		*conf_mode = ft4222_conf3;
		break;
	case 0x18:
		*conf_mode = ft4222_conf0;
		break;
	case 0x19:
		*conf_mode = ft4222_conf12;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int ft4222_i2c_probe(struct usb_interface *interface,
			    const struct usb_device_id *id)
{
	enum ft4222_conf_mode conf_mode;
	int ret = ft4222_get_conf(interface, &conf_mode);
	int intf = interface->cur_altsetting->desc.bInterfaceNumber;

	if (ret)
		return ret;
	if (((conf_mode == ft4222_conf0) || (conf_mode == ft4222_conf3)) &&
	    (intf == 0))
		return ft4222_i2c_init(interface);
	return -ENODEV;
}

static void ft4222_i2c_disconnect(struct usb_interface *interface)
{
}

static const struct usb_device_id ft4222_id_table[] = {
	{ USB_DEVICE(0x0403, 0x601C) },
	{ }
};
MODULE_DEVICE_TABLE(usb, ft4222_id_table);

static struct usb_driver ft4222_i2c_usb_driver = {
	.name = "i2c-ft4222",
	.probe = ft4222_i2c_probe,
	.disconnect = ft4222_i2c_disconnect,
	.id_table = ft4222_id_table,
};

module_usb_driver(ft4222_i2c_usb_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FT4222H USB-to-I2C bridge");
