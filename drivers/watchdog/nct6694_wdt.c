// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 WDT driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/watchdog.h>

#define DRVNAME "nct6694-wdt"

#define NCT6694_DEFAULT_TIMEOUT		10
#define NCT6694_DEFAULT_PRETIMEOUT	0

/* Host interface */
#define NCT6694_WDT_MOD		0x07

/* Message Channel*/
/* Command 00h */
#define NCT6694_WDT_CMD0_LEN	0x0F
#define NCT6694_WDT_CMD0_OFFSET(idx)	(idx ? 0x0100 : 0x0000)	/* OFFSET = SEL|CMD */

/* Command 01h */
#define NCT6694_WDT_CMD1_LEN		0x08
#define NCT6694_WDT_CMD1_OFFSET(idx)	(idx ? 0x0101 : 0x0001)	/* OFFSET = SEL|CMD */

static unsigned int timeout = NCT6694_DEFAULT_TIMEOUT;
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout, "Watchdog timeout in seconds");

static unsigned int pretimeout = NCT6694_DEFAULT_PRETIMEOUT;
module_param(pretimeout, int, 0);
MODULE_PARM_DESC(pretimeout, "Watchdog pre-timeout in seconds");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
			   __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

enum {
	NCT6694_ACTION_NONE = 0,
	NCT6694_ACTION_SIRQ,
	NCT6694_ACTION_GPO,
};

struct __packed nct6694_wdt_cmd0 {
	__le32 pretimeout;
	__le32 timeout;
	u8 owner;
	u8 scratch;
	u8 control;
	u8 status;
	__le32 countdown;
};

struct __packed nct6694_wdt_cmd1 {
	u32 wdt_cmd;
	u32 reserved;
};

struct nct6694_wdt_data {
	struct watchdog_device wdev;
	struct device *dev;
	struct nct6694 *nct6694;
	struct mutex lock;
	unsigned char *xmit_buf;
	unsigned int wdev_idx;
};

static int nct6694_wdt_setting(struct watchdog_device *wdev,
			       u32 timeout_val, u8 timeout_act,
			       u32 pretimeout_val, u8 pretimeout_act)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694_wdt_cmd0 *buf = (struct nct6694_wdt_cmd0 *)data->xmit_buf;
	struct nct6694 *nct6694 = data->nct6694;
	unsigned int timeout_fmt, pretimeout_fmt;

	guard(mutex)(&data->lock);

	if (pretimeout_val == 0)
		pretimeout_act = NCT6694_ACTION_NONE;

	timeout_fmt = (timeout_val * 1000) | (timeout_act << 24);
	pretimeout_fmt = (pretimeout_val * 1000) | (pretimeout_act << 24);

	memset(buf, 0, NCT6694_WDT_CMD0_LEN);
	buf->timeout = cpu_to_le32(timeout_fmt);
	buf->pretimeout = cpu_to_le32(pretimeout_fmt);

	return nct6694_write_msg(nct6694, NCT6694_WDT_MOD,
				 NCT6694_WDT_CMD0_OFFSET(data->wdev_idx),
				 NCT6694_WDT_CMD0_LEN, buf);
}

static int nct6694_wdt_start(struct watchdog_device *wdev)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	int ret;

	ret = nct6694_wdt_setting(wdev, wdev->timeout, NCT6694_ACTION_GPO,
				  wdev->pretimeout, NCT6694_ACTION_GPO);
	if (ret)
		return ret;

	dev_info(data->dev, "Setting WDT(%d): timeout = %d, pretimeout = %d\n",
		 data->wdev_idx, wdev->timeout, wdev->pretimeout);

	return ret;
}

static int nct6694_wdt_stop(struct watchdog_device *wdev)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694_wdt_cmd1 *buf = (struct nct6694_wdt_cmd1 *)data->xmit_buf;
	struct nct6694 *nct6694 = data->nct6694;

	guard(mutex)(&data->lock);

	memcpy(buf, "WDTC", 4);
	buf->reserved = 0;

	return nct6694_write_msg(nct6694, NCT6694_WDT_MOD,
				 NCT6694_WDT_CMD1_OFFSET(data->wdev_idx),
				 NCT6694_WDT_CMD1_LEN, buf);
}

static int nct6694_wdt_ping(struct watchdog_device *wdev)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694_wdt_cmd1 *buf = (struct nct6694_wdt_cmd1 *)data->xmit_buf;
	struct nct6694 *nct6694 = data->nct6694;

	guard(mutex)(&data->lock);
	memcpy(buf, "WDTS", 4);
	buf->reserved = 0;

	return nct6694_write_msg(nct6694, NCT6694_WDT_MOD,
				 NCT6694_WDT_CMD1_OFFSET(data->wdev_idx),
				 NCT6694_WDT_CMD1_LEN, buf);
}

static int nct6694_wdt_set_timeout(struct watchdog_device *wdev,
				   unsigned int timeout)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	int ret;

	if (timeout < wdev->pretimeout) {
		dev_warn(data->dev, "pretimeout < timeout. Setting to zero\n");
		wdev->pretimeout = 0;
	}

	ret = nct6694_wdt_setting(wdev, timeout, NCT6694_ACTION_GPO,
				  wdev->pretimeout, NCT6694_ACTION_GPO);
	if (ret)
		return ret;

	wdev->timeout = timeout;

	return ret;
}

static int nct6694_wdt_set_pretimeout(struct watchdog_device *wdev,
				      unsigned int pretimeout)
{
	int ret;

	ret = nct6694_wdt_setting(wdev, wdev->timeout, NCT6694_ACTION_GPO,
				  pretimeout, NCT6694_ACTION_GPO);
	if (ret)
		return ret;

	wdev->pretimeout = pretimeout;

	return ret;
}

static unsigned int nct6694_wdt_get_time(struct watchdog_device *wdev)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694_wdt_cmd0 *buf = (struct nct6694_wdt_cmd0 *)data->xmit_buf;
	struct nct6694 *nct6694 = data->nct6694;
	unsigned int timeleft_ms;
	int ret;

	guard(mutex)(&data->lock);

	ret = nct6694_read_msg(nct6694, NCT6694_WDT_MOD,
			       NCT6694_WDT_CMD0_OFFSET(data->wdev_idx),
			       NCT6694_WDT_CMD0_LEN, buf);
	if (ret)
		return ret;

	timeleft_ms = le32_to_cpu(buf->countdown);

	return timeleft_ms / 1000;
}

static const struct watchdog_info nct6694_wdt_info = {
	.options = WDIOF_SETTIMEOUT	|
		   WDIOF_KEEPALIVEPING	|
		   WDIOF_MAGICCLOSE	|
		   WDIOF_PRETIMEOUT,
	.identity = DRVNAME,
};

static const struct watchdog_ops nct6694_wdt_ops = {
	.owner = THIS_MODULE,
	.start = nct6694_wdt_start,
	.stop = nct6694_wdt_stop,
	.set_timeout = nct6694_wdt_set_timeout,
	.set_pretimeout = nct6694_wdt_set_pretimeout,
	.get_timeleft = nct6694_wdt_get_time,
	.ping = nct6694_wdt_ping,
};

static int nct6694_wdt_probe(struct platform_device *pdev)
{
	const struct mfd_cell *cell = mfd_get_cell(pdev);
	struct device *dev = &pdev->dev;
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	struct nct6694_wdt_data *data;
	struct watchdog_device *wdev;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->xmit_buf = devm_kcalloc(dev, NCT6694_MAX_PACKET_SZ,
				      sizeof(unsigned char), GFP_KERNEL);
	if (!data->xmit_buf)
		return -ENOMEM;

	data->dev = dev;
	data->nct6694 = nct6694;
	data->wdev_idx = cell->id;

	wdev = &data->wdev;
	wdev->info = &nct6694_wdt_info;
	wdev->ops = &nct6694_wdt_ops;
	wdev->timeout = timeout;
	wdev->pretimeout = pretimeout;
	wdev->min_timeout = 1;
	wdev->max_timeout = 255;

	mutex_init(&data->lock);

	platform_set_drvdata(pdev, data);

	/* Register watchdog timer device to WDT framework */
	watchdog_set_drvdata(&data->wdev, data);
	watchdog_init_timeout(&data->wdev, timeout, dev);
	watchdog_set_nowayout(&data->wdev, nowayout);
	watchdog_stop_on_reboot(&data->wdev);

	return devm_watchdog_register_device(dev, &data->wdev);
}

static struct platform_driver nct6694_wdt_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_wdt_probe,
};

module_platform_driver(nct6694_wdt_driver);

MODULE_DESCRIPTION("USB-WDT driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
