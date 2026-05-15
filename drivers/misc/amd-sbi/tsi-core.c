// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tsi-core.c - file defining SB-TSI protocols compliant
 *              AMD SoC device.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <uapi/misc/amd-apml.h>
#include "tsi-core.h"

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

/* I3C read transfer function */
static int sbtsi_i3c_read(struct sbtsi_data *data, u8 reg, u8 *val)
{
	struct i3c_xfer xfers[2] = { };

	/* Add Register data to read/write */
	xfers[0].rnw = false;
	xfers[0].len = 1;
	xfers[0].data.out = &reg;

	xfers[1].rnw = true;
	xfers[1].len = 1;
	xfers[1].data.in = val;

	return i3c_device_do_xfers(data->i3cdev, xfers, 2, I3C_SDR);
}

/* I3C write transfer function */
static int sbtsi_i3c_write(struct sbtsi_data *data, u8 reg, u8 val)
{
	u8 buf[2];
	struct i3c_xfer xfers = {
		.rnw = false,
		.len = 2,
		.data.out = buf,
	};

	buf[0] = reg;
	buf[1] = val;

	return i3c_device_do_xfers(data->i3cdev, &xfers, 1, I3C_SDR);
}

/* Unified transfer function for I2C and I3C access */
int sbtsi_xfer(struct sbtsi_data *data, u8 reg, u8 *val, bool is_read)
{
	if (data->is_i3c)
		return is_read ? sbtsi_i3c_read(data, reg, val)
			       : sbtsi_i3c_write(data, reg, *val);
	return sbtsi_i2c_xfer(data, reg, val, is_read);
}
EXPORT_SYMBOL_GPL(sbtsi_xfer);

/*
 * The mutex protects against concurrent access to the shared I2C/I3C bus by
 * the hwmon sysfs and a userspace ioctl.
 */
static int sbtsi_xfer_ioctl(struct sbtsi_data *data, u8 reg, u8 *val, bool is_read)
{
	guard(mutex)(&data->lock);
	return sbtsi_xfer(data, reg, val, is_read);
}

static int apml_tsi_reg_xfer(struct sbtsi_data *data,
			     struct apml_tsi_xfer_msg __user *arg)
{
	struct apml_tsi_xfer_msg msg = { 0 };
	int ret;

	if (copy_from_user(&msg, arg, sizeof(struct apml_tsi_xfer_msg)))
		return -EFAULT;

	ret = sbtsi_xfer_ioctl(data, msg.reg_addr, &msg.data_in_out, msg.rflag);

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
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= sbtsi_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static void sbtsi_misc_deregister(void *data)
{
	misc_deregister((struct miscdevice *)data);
}

int create_misc_tsi_device(struct sbtsi_data *data, struct device *dev)
{
	int ret;

	data->sbtsi_misc_dev.name = devm_kasprintf(dev, GFP_KERNEL,
						   "sbtsi-%x", data->dev_addr);
	if (!data->sbtsi_misc_dev.name)
		return -ENOMEM;
	data->sbtsi_misc_dev.minor    = MISC_DYNAMIC_MINOR;
	data->sbtsi_misc_dev.fops     = &sbtsi_fops;
	data->sbtsi_misc_dev.parent   = dev;
	data->sbtsi_misc_dev.nodename = devm_kasprintf(dev, GFP_KERNEL,
						       "sbtsi-%x", data->dev_addr);
	if (!data->sbtsi_misc_dev.nodename)
		return -ENOMEM;
	data->sbtsi_misc_dev.mode = 0600;

	ret = misc_register(&data->sbtsi_misc_dev);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, sbtsi_misc_deregister,
					&data->sbtsi_misc_dev);
}
