// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026, Altera Corporation
 */

#include <linux/err.h>
#include <linux/firmware/intel/socfpga-fcs.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/util_macros.h>
#include <uapi/misc/socfpga-fcs-crypto.h>

/**
 * atf_version_show() - report the Arm Trusted Firmware build version
 * @dev: pointer to fcs device
 * @attr: device attribute
 * @buf: pointer to character buffer to receive the version string
 *
 * Return: number of bytes written to @buf.
 */
static ssize_t atf_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct socfpga_fcs_priv *priv = dev_get_drvdata(dev);
	u32 version[3];
	int ret;

	ret = fcs_get_atf_version(priv, version);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u.%u.%u\n", version[0], version[1], version[2]);
}

/**
 * fcs_sdos() - perform an SDOS encrypt/decrypt operation
 * @priv: FCS state of the device this request arrived on
 * @uarg: user pointer to a struct fcs_ioc_sdos
 *
 * Return: 0 on success, negative errno on failure.
 */
static long fcs_sdos(struct socfpga_fcs_priv *priv, void __user *uarg)
{
	u32 __user *dst_size_uptr;
	s32 __user *status_uptr;
	struct fcs_sdos_req req = { };
	struct fcs_ioc_sdos u;
	void *s_buf, *d_buf;
	u32 output_size;
	u32 dst_cap;
	long ret;

	if (copy_from_user(&u, uarg, sizeof(u)))
		return -EFAULT;

	if (!u.dst || !u.dst_size)
		return -EINVAL;

	dst_size_uptr = u64_to_user_ptr(u.dst_size);
	status_uptr = u64_to_user_ptr(u.error_code);

	/* Caller-provided output buffer capacity (in/out parameter) */
	if (get_user(dst_cap, dst_size_uptr))
		return -EFAULT;

	ret = fcs_sdos_output_size(u.op_mode, u.src_size, &output_size);
	if (ret)
		return ret;

	s_buf = fcs_alloc_buf(priv, u.src_size);
	if (IS_ERR(s_buf))
		return PTR_ERR(s_buf);

	d_buf = fcs_alloc_buf(priv, output_size);
	if (IS_ERR(d_buf)) {
		ret = PTR_ERR(d_buf);
		goto free_sbuf;
	}

	/*
	 * Copy before the engine takes its lock, so a slow or faulting source
	 * buffer cannot stall unrelated FCS callers.
	 */
	if (copy_from_user(s_buf, u64_to_user_ptr(u.src), u.src_size)) {
		ret = -EFAULT;
		goto free_dbuf;
	}

	req.op_mode	= u.op_mode;
	req.src		= s_buf;
	req.src_len	= u.src_size;
	req.dst		= d_buf;
	req.dst_len	= output_size;

	ret = fcs_sdos_crypt(priv, &req);
	if (ret)
		goto relay_status;

	if (req.dst_len > dst_cap) {
		pr_debug("SDOS output %u exceeds caller buffer %u\n",
			 req.dst_len, dst_cap);
		ret = -EMSGSIZE;
		goto relay_status;
	}

	if (copy_to_user(u64_to_user_ptr(u.dst), d_buf, req.dst_len)) {
		ret = -EFAULT;
		goto relay_status;
	}

	if (put_user(req.dst_len, dst_size_uptr))
		ret = -EFAULT;

relay_status:
	if (req.status_valid && put_user(req.status, status_uptr)) {
		/* surface the copy failure only if nothing failed earlier */
		if (!ret)
			ret = -EFAULT;
	}
free_dbuf:
	fcs_free_buf(priv, d_buf);
free_sbuf:
	fcs_free_buf(priv, s_buf);

	return ret;
}

/**
 * fcs_open() - take a reference on the device state for this file
 * @inode: inode of the FCS misc device
 * @file: open file being created
 *
 * Return: 0 always.
 */
static int fcs_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct socfpga_fcs_priv *priv =
		container_of(miscdev, struct socfpga_fcs_priv, miscdev);

	fcs_get(priv);
	file->private_data = priv;

	return 0;
}

/**
 * fcs_release() - drop this file's reference on the device state to
 * guarantees the crypto session is torn down when its owning fd is closed,
 * including on process crash/exit
 * @inode: inode of the FCS misc device
 * @file: open file being released
 *
 * Return: 0 always.
 */
static int fcs_release(struct inode *inode, struct file *file)
{
	fcs_put(file->private_data);

	return 0;
}

/**
 * fcs_ioctl() - dispatch an FCS ioctl command
 * @file: open file for the FCS misc device
 * @cmd: ioctl command code
 * @arg: user pointer to the command-specific argument structure
 *
 * Return: 0 on success, -ENOTTY for an unknown command, or a negative errno
 *         from the handler.
 */
static long fcs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct socfpga_fcs_priv *priv = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case FCS_IOC_SDOS:
		return fcs_sdos(priv, uarg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations fcs_fops = {
	.owner		= THIS_MODULE,
	.open		= fcs_open,
	.release	= fcs_release,
	.unlocked_ioctl	= fcs_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static DEVICE_ATTR_RO(atf_version);

static struct attribute *fcs_attrs[] = {
	&dev_attr_atf_version.attr,
	NULL
};
ATTRIBUTE_GROUPS(fcs);

/**
 * fcs_driver_probe() - probe the FCS platform device
 * @pdev: pointer to the FCS platform device
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct socfpga_fcs_priv *priv;
	int ret;

	priv = fcs_init(dev);
	if (IS_ERR(priv))
		return dev_err_probe(dev, PTR_ERR(priv),
				     "Failed to initialize FCS\n");

	platform_set_drvdata(pdev, priv);

	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.name = "socfpga-fcs";
	priv->miscdev.fops = &fcs_fops;
	priv->miscdev.parent = dev;

	ret = misc_register(&priv->miscdev);
	if (ret) {
		fcs_put(priv);
		return dev_err_probe(dev, ret, "Failed to register misc device\n");
	}

	return 0;
}

/**
 * fcs_driver_remove() - remove the FCS platform device
 * @pdev: pointer to the FCS platform device
 */
static void fcs_driver_remove(struct platform_device *pdev)
{
	struct socfpga_fcs_priv *priv = platform_get_drvdata(pdev);

	/*
	 * misc_deregister() does not wait for open files. Drop the driver's
	 * reference; the channel lives until the last close if any remain.
	 */
	misc_deregister(&priv->miscdev);
	fcs_mark_removed(priv);
	fcs_put(priv);
}

static struct platform_driver fcs_driver = {
	.probe = fcs_driver_probe,
	.remove = fcs_driver_remove,
	.driver = {
		.name = "stratix10-fcs",
		.dev_groups = fcs_groups,
	},
};

/**
 * socfpga_fcs_init() - register the FCS platform driver
 *
 * Return: 0 on success, negative errno on failure.
 */
static int __init socfpga_fcs_init(void)
{
	int ret;

	ret = platform_driver_register(&fcs_driver);
	if (ret)
		pr_err("Failed to register platform driver: %d\n", ret);

	return ret;
}

/**
 * socfpga_fcs_exit() - unregister the FCS platform driver
 */
static void __exit socfpga_fcs_exit(void)
{
	platform_driver_unregister(&fcs_driver);
}

module_init(socfpga_fcs_init);
module_exit(socfpga_fcs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Altera SoCFPGA FCS SDOS encrypt/decrypt driver");
MODULE_AUTHOR("Altera Corporation");
MODULE_ALIAS("platform:stratix10-fcs");
