// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * WMI platform integration for Dell All-In-One series.
 *
 * Copyright (C) 2026 Armin Wolf <W_Armin@gmx.de>
 *
 * Based on earlier work by Teodora <teolyub@gmail.com>.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/dev_printk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wmi.h>

#define PEGA_GUID	"2D703EE8-36AC-4F88-BDB5-7556CE6B2946"

enum dell_pega_method {
	DELL_PEGA_NOTIFICATION_STARTUP		= 0x01,
	DELL_PEGA_NOTIFICATION_STOP		= 0x02,
	DELL_PEGA_NOTIFICATION_BRIGHTNESS_UP	= 0x03,
	DELL_PEGA_NOTIFICATION_BRIGHTNESS_DOWN	= 0x04,
	DELL_PEGA_NOTIFICATION_RESERVED_1	= 0x05,
	DELL_PEGA_NOTIFICATION_RESERVED_2	= 0x06,
};

static int dell_wmi_pega_invoke(struct wmi_device *wdev, enum dell_pega_method method)
{
	struct wmi_buffer input = { };
	struct wmi_buffer output;
	int ret;

	ret = wmidev_invoke_method(wdev, 0x0, method, &input, &output, sizeof(__le32));
	if (ret < 0)
		return ret;

	__le32 *result __free(kfree) = output.data;
	/* The return value should always be equal to the WMI method ID */
	if (le32_to_cpu(*result) != method)
		return -EIO;

	return 0;
}

static int dell_wmi_pega_probe(struct wmi_device *wdev, const void *context)
{
	int ret;

	ret = dell_wmi_pega_invoke(wdev, DELL_PEGA_NOTIFICATION_STARTUP);
	if (ret < 0)
		dev_err(&wdev->dev,
			"Failed to notify platform of startup, some hotkeys might not work\n");

	return ret;
}

static void dell_wmi_pega_shutdown(struct wmi_device *wdev)
{
	int ret;

	ret = dell_wmi_pega_invoke(wdev, DELL_PEGA_NOTIFICATION_STOP);
	if (ret < 0)
		dev_warn(&wdev->dev, "Failed to notify platform of shutdown\n");
}

static const struct wmi_device_id dell_wmi_pega_id_table[] = {
	{ PEGA_GUID, NULL },
	{ }
};
MODULE_DEVICE_TABLE(wmi, dell_wmi_pega_id_table);

static struct wmi_driver dell_wmi_pega_driver = {
	.driver = {
		.name = "dell-wmi-pega",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = dell_wmi_pega_id_table,
	.probe = dell_wmi_pega_probe,
	.remove = dell_wmi_pega_shutdown,
	.shutdown = dell_wmi_pega_shutdown,
	.no_singleton = true,
};
module_wmi_driver(dell_wmi_pega_driver);

MODULE_AUTHOR("Armin Wolf <W_Armin@gmx.de>");
MODULE_DESCRIPTION("Dell All-In-One series platform integration");
MODULE_LICENSE("GPL");
