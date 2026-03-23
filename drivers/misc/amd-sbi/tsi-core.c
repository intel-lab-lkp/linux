// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tsi-core.c - file defining SB-TSI protocols compliant
 *              AMD SoC device.
 * Copyright (c) 2020, Google Inc.
 * Copyright (c) 2020, Kun Yi <kunyi@google.com>
 */

#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>
#include <uapi/misc/amd-apml.h>
#include "tsi-core.h"

/* I3C read transfer function */
static int sbtsi_i3c_read(struct sbtsi_data *data, u8 reg, u8 *val)
{
	struct i3c_xfer xfers[2];

	/* Send register address */
	xfers[0].rnw = false;
	xfers[0].len = 1;
	xfers[0].data.out = &reg;

	/* Read data */
	xfers[1].rnw = true;
	xfers[1].len = 1;
	xfers[1].data.in = val;

	return i3c_device_do_xfers(data->i3cdev, xfers, 2, I3C_SDR);
}

/* I3C write transfer function */
static int sbtsi_i3c_write(struct sbtsi_data *data, u8 reg, u8 *val)
{
	u8 buf[2] = { reg, *val };
	struct i3c_xfer xfers = {
		.rnw = false,
		.len = 2,
		.data.out = buf,
	};

	return i3c_device_do_xfers(data->i3cdev, &xfers, 1, I3C_SDR);
}

/* I2C transfer function */
static int sbtsi_i2c_xfer(struct sbtsi_data *data, u8 reg, u8 *val, bool is_read)
{
	if (is_read) {
		int ret = i2c_smbus_read_byte_data(data->client, reg);

		if (ret < 0)
			return ret;
		*val = ret;
		return 0;
	}
	return i2c_smbus_write_byte_data(data->client, reg, *val);
}

/* Unified transfer function for I2C and I3C access */
int sbtsi_xfer(struct sbtsi_data *data, u8 reg, u8 *val, bool is_read)
{
	int ret;

	mutex_lock(&data->lock);
	if (data->is_i3c)
		ret = is_read ? sbtsi_i3c_read(data, reg, val)
			: sbtsi_i3c_write(data, reg, val);
	else
		ret = sbtsi_i2c_xfer(data, reg, val, is_read);
	mutex_unlock(&data->lock);
	return ret;
}

static int apml_tsi_reg_xfer(struct sbtsi_data *data,
			     struct apml_tsi_xfer_msg __user *arg)
{
	struct apml_tsi_xfer_msg msg = { 0 };
	u8 val;
	int ret;

	/* Copy the structure from user */
	if (copy_from_user(&msg, arg, sizeof(struct apml_tsi_xfer_msg)))
		return -EFAULT;

	if (msg.rflag) {
		/* Read operation */
		ret = sbtsi_xfer(data, msg.reg_addr, &val, true);
		if (!ret)
			msg.data_in_out = val;
	} else {
		/* Write operation */
		ret = sbtsi_xfer(data, msg.reg_addr, &msg.data_in_out, false);
	}

	if (msg.rflag && !ret) {
		if (copy_to_user(arg, &msg, sizeof(struct apml_tsi_xfer_msg)))
			return -EFAULT;
	}
	return ret;
}

static long sbtsi_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct sbtsi_data *data;

	data = container_of(fp->private_data, struct sbtsi_data, sbtsi_misc_dev);
	switch (cmd) {
	case SBTSI_IOCTL_REG_XFER_CMD:
		return apml_tsi_reg_xfer(data, argp);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations sbtsi_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = sbtsi_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

int create_misc_tsi_device(struct sbtsi_data *data,
			   struct device *dev)
{
	int ret;

	data->sbtsi_misc_dev.name            = devm_kasprintf(dev, GFP_KERNEL,
							      "sbtsi-%x", data->dev_addr);
	if (!data->sbtsi_misc_dev.name)
		return -ENOMEM;
	data->sbtsi_misc_dev.minor           = MISC_DYNAMIC_MINOR;
	data->sbtsi_misc_dev.fops            = &sbtsi_fops;
	data->sbtsi_misc_dev.parent          = dev;
	data->sbtsi_misc_dev.nodename        = devm_kasprintf(dev, GFP_KERNEL,
							      "sbtsi-%x", data->dev_addr);
	if (!data->sbtsi_misc_dev.nodename)
		return -ENOMEM;
	data->sbtsi_misc_dev.mode            = 0600;

	ret = misc_register(&data->sbtsi_misc_dev);
	if (ret)
		return ret;

	return ret;
}
