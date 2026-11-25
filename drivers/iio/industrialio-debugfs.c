// SPDX-License-Identifier: GPL-2.0-only
/*
 * Industrial I/O debugfs bits
 */

#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/dev_printk.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/sprintf.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include <linux/iio/iio.h>
#include <linux/iio/iio-opaque.h>

#include "iio_core.h"

static struct dentry *iio_debugfs_dentry;

/*
 * There's also a CONFIG_DEBUG_FS guard in include/linux/iio/iio.h for
 * iio_get_debugfs_dentry() to make it inline if CONFIG_DEBUG_FS is undefined
 */
struct dentry *iio_get_debugfs_dentry(struct iio_dev *indio_dev)
{
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(indio_dev);

	return iio_dev_opaque->debugfs_dentry;
}
EXPORT_SYMBOL_GPL(iio_get_debugfs_dentry);

void __init iio_debugfs_create(void)
{
	iio_debugfs_dentry = debugfs_create_dir("iio", NULL);
}

void __exit iio_debugfs_remove(void)
{
	debugfs_remove(iio_debugfs_dentry);
}

static ssize_t iio_debugfs_read_reg(struct file *file, char __user *userbuf,
			      size_t count, loff_t *ppos)
{
	struct iio_dev *indio_dev = file->private_data;
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(indio_dev);
	unsigned int val = 0;
	u64 val64 = 0;
	int ret;

	if (*ppos > 0)
		return simple_read_from_buffer(userbuf, count, ppos,
					       iio_dev_opaque->read_buf,
					       iio_dev_opaque->read_buf_len);

	if (indio_dev->info->debugfs_reg64_access) {
		ret = indio_dev->info->debugfs_reg64_access(indio_dev,
							    iio_dev_opaque->cached_reg_addr,
							    0, &val64);
	} else {
		ret = indio_dev->info->debugfs_reg_access(indio_dev,
							  iio_dev_opaque->cached_reg_addr,
							  0, &val);
		val64 = val;
	}

	if (ret) {
		dev_err(indio_dev->dev.parent, "%s: read failed\n", __func__);
		return ret;
	}

	iio_dev_opaque->read_buf_len = snprintf(iio_dev_opaque->read_buf,
						sizeof(iio_dev_opaque->read_buf),
						"0x%llX\n", val64);

	return simple_read_from_buffer(userbuf, count, ppos,
				       iio_dev_opaque->read_buf,
				       iio_dev_opaque->read_buf_len);
}

static ssize_t iio_debugfs_write_reg(struct file *file,
		     const char __user *userbuf, size_t count, loff_t *ppos)
{
	struct iio_dev *indio_dev = file->private_data;
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(indio_dev);
	unsigned int reg;
	char buf[80];
	u64 val64;
	int ret;

	if (*ppos != 0 || count >= sizeof(buf))
		return -EINVAL;

	ret = simple_write_to_buffer(buf, sizeof(buf) - 1, ppos, userbuf,
				     count);
	if (ret < 0)
		return ret;

	buf[ret] = '\0';

	ret = sscanf(buf, "%i %lli", &reg, &val64);

	switch (ret) {
	case 1:
		iio_dev_opaque->cached_reg_addr = reg;
		break;
	case 2:
		iio_dev_opaque->cached_reg_addr = reg;
		if (indio_dev->info->debugfs_reg64_access)
			ret = indio_dev->info->debugfs_reg64_access(indio_dev, reg,
								    val64, NULL);
		else
			ret = indio_dev->info->debugfs_reg_access(indio_dev, reg,
								  val64, NULL);
		if (ret) {
			dev_err(indio_dev->dev.parent, "%s: write failed\n",
				__func__);
			return ret;
		}
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static const struct file_operations iio_debugfs_reg_fops = {
	.open = simple_open,
	.read = iio_debugfs_read_reg,
	.write = iio_debugfs_write_reg,
};

void iio_device_unregister_debugfs(struct iio_dev *indio_dev)
{
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(indio_dev);

	debugfs_remove_recursive(iio_dev_opaque->debugfs_dentry);
}

void iio_device_register_debugfs(struct iio_dev *indio_dev)
{
	struct iio_dev_opaque *iio_dev_opaque;

	if (!indio_dev->info->debugfs_reg_access &&
	    !indio_dev->info->debugfs_reg64_access)
		return;

	if (!iio_debugfs_dentry)
		return;

	iio_dev_opaque = to_iio_dev_opaque(indio_dev);

	iio_dev_opaque->debugfs_dentry =
		debugfs_create_dir(dev_name(&indio_dev->dev),
				   iio_debugfs_dentry);

	debugfs_create_file("direct_reg_access", 0644,
			    iio_dev_opaque->debugfs_dentry, indio_dev,
			    &iio_debugfs_reg_fops);
}
