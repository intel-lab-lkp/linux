// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026, Altera Corporation
 */

#include <linux/firmware/intel/socfpga-fcs.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

/**
 * open_session_store() - open an FCS crypto service session
 * @dev: pointer to fcs device
 * @attr: device attribute
 * @buf: pointer to character buffer carrying the user command context
 * @buf_size: size of the buffer
 *
 * Return: @buf_size on success, negative errno on failure.
 */
static ssize_t open_session_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t buf_size)
{
	struct fcs_cmd_context *const u_ctx = *(struct fcs_cmd_context **)buf;
	struct fcs_cmd_context *k_ctx;
	int ret;

	k_ctx = fcs_acquire_cmd_ctx();
	if (!k_ctx) {
		dev_err(dev, "Failed get context. Context is in use\n");
		return -EBUSY;
	}

	if (copy_from_user(k_ctx, u_ctx, sizeof(*k_ctx))) {
		dev_err(dev, "Failed to copy context from user space\n");
		ret = -EFAULT;
		goto out;
	}

	ret = fcs_session_open(k_ctx);
	if (ret) {
		dev_err(dev, "Failed to open session\n");
		goto out;
	}

	ret = buf_size;
out:
	fcs_release_cmd_ctx(k_ctx);

	return ret;
}

/**
 * close_session_store() - close an FCS crypto service session
 * @dev: pointer to fcs device
 * @attr: device attribute
 * @buf: pointer to character buffer carrying the user command context
 * @buf_size: size of the buffer
 *
 * Return: @buf_size on success, negative errno on failure.
 */
static ssize_t close_session_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t buf_size)
{
	struct fcs_cmd_context *const u_ctx = *(struct fcs_cmd_context **)buf;
	struct fcs_cmd_context *k_ctx;
	int ret;

	k_ctx = fcs_acquire_cmd_ctx();
	if (!k_ctx) {
		dev_err(dev, "Failed get context. Context is in use\n");
		return -EBUSY;
	}

	if (copy_from_user(k_ctx, u_ctx, sizeof(*k_ctx))) {
		dev_err(dev, "Failed to copy context from user space\n");
		ret = -EFAULT;
		goto out;
	}

	ret = fcs_session_close(k_ctx);
	if (ret) {
		dev_err(dev, "Failed to close session\n");
		goto out;
	}

	ret = buf_size;
out:
	fcs_release_cmd_ctx(k_ctx);

	return ret;
}

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
	int version[3];

	fcs_get_atf_version(version);
	return sysfs_emit(buf, "%u.%u.%u\n", version[0], version[1], version[2]);
}

/**
 * sdos_store() - perform an SDOS encrypt/decrypt operation
 * @dev: pointer to fcs device
 * @attr: device attribute
 * @buf: pointer to character buffer carrying the user command context
 * @buf_size: size of the buffer
 *
 * Return: @buf_size on success, negative errno on failure.
 */
static ssize_t sdos_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t buf_size)
{
	struct fcs_cmd_context *const u_ctx = *(struct fcs_cmd_context **)buf;
	struct fcs_cmd_context *k_ctx;
	int ret;

	k_ctx = fcs_acquire_cmd_ctx();
	if (!k_ctx) {
		dev_err(dev, "Failed get context. Context is in use\n");
		return -EBUSY;
	}

	if (copy_from_user(k_ctx, u_ctx, sizeof(*k_ctx))) {
		dev_err(dev, "Failed to copy context from user space\n");
		ret = -EFAULT;
		goto out;
	}

	ret = fcs_sdos_crypt(k_ctx);
	if (ret) {
		dev_err(dev, "Failed to perform SDOS operation\n");
		goto out;
	}

	ret = buf_size;
out:
	fcs_release_cmd_ctx(k_ctx);

	return ret;
}

static DEVICE_ATTR_WO(open_session);
static DEVICE_ATTR_WO(close_session);
static DEVICE_ATTR_RO(atf_version);
static DEVICE_ATTR_WO(sdos);

static struct attribute *fcs_attrs[] = {
	&dev_attr_open_session.attr,
	&dev_attr_close_session.attr,
	&dev_attr_atf_version.attr,
	&dev_attr_sdos.attr,
	NULL
};

static struct attribute_group fcs_group = {
	.attrs = fcs_attrs,
};

static const struct attribute_group *fcs_groups[] = {
	&fcs_group,
	NULL,
};

/**
 * fcs_driver_probe() - probe the FCS platform device
 * @pdev: pointer to the FCS platform device
 *
 * Initialise the FCS state. The sysfs attribute groups are published
 * automatically by the driver core via fcs_driver.driver.dev_groups.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	ret = fcs_init(dev);
	if (ret) {
		dev_err(dev, "Failed to initialize FCS\n");
		return ret;
	}

	return 0;
}

/**
 * fcs_driver_remove() - remove the FCS platform device
 * @pdev: pointer to the FCS platform device
 *
 * Tear down the FCS state. The sysfs attribute groups are removed
 * automatically by the driver core.
 */
static void fcs_driver_remove(struct platform_device *pdev)
{
	fcs_deinit();
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
