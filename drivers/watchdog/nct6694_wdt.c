// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 WDT driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/watchdog.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/core.h>
#include <linux/platform_device.h>
#include <linux/mfd/nct6694.h>

#define DRVNAME "nct6694-wdt"

#define WATCHDOG_TIMEOUT	10
#define WATCHDOG_PRETIMEOUT	0

/* Host interface */
#define REQUEST_WDT_MOD		0x07

/* Message Channel*/
/* Command 00h */
#define REQUEST_WDT_CMD0_LEN	0x0F
#define REQUEST_WDT_CMD0_OFFSET(idx)	(idx ? 0x0100 : 0x0000)	/* OFFSET = SEL|CMD */
#define WDT_PRETIMEOUT_IDX	0x00
#define WDT_PRETIMEOUT_LEN	0x04	/* PRETIMEOUT(3byte) | ACT(1byte) */
#define WDT_TIMEOUT_IDX		0x04
#define WDT_TIMEOUT_LEN		0x04	/* TIMEOUT(3byte) | ACT(1byte) */
#define WDT_COUNTDOWN_IDX	0x0C
#define WDT_COUNTDOWN_LEN	0x03

#define WDT_PRETIMEOUT_ACT	BIT(1)
#define WDT_TIMEOUT_ACT		BIT(1)

/* Command 01h */
#define REQUEST_WDT_CMD1_LEN		0x04
#define REQUEST_WDT_CMD1_OFFSET(idx)	(idx ? 0x0101 : 0x0001)	/* OFFSET = SEL|CMD */
#define WDT_CMD_IDX			0x00
#define WDT_CMD_LEN			0x04

static unsigned int timeout;
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout, "Watchdog timeout in seconds");

static unsigned int pretimeout;
module_param(pretimeout, int, 0);
MODULE_PARM_DESC(pretimeout, "Watchdog pre-timeout in seconds");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
			   __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct nct6694_wdt_data {
	struct nct6694 *nct6694;
	struct watchdog_device wdev;
	unsigned int wdev_idx;
};

static inline void set_buf32(void *buf, u32 u32_val)
{
	u8 *p = (u8 *)buf;

	p[0] = u32_val & 0xFF;
	p[1] = (u32_val >> 8) & 0xFF;
	p[2] = (u32_val >> 16) & 0xFF;
	p[3] = (u32_val >> 24) & 0xFF;
}

static int nct6694_wdt_start(struct watchdog_device *wdev)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);

	pr_debug("%s: WDT(%d) Start\n", __func__, data->wdev_idx);

	return 0;
}

static int nct6694_wdt_stop(struct watchdog_device *wdev)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694 *nct6694 = data->nct6694;
	unsigned char buf[REQUEST_WDT_CMD1_LEN] = {'W', 'D', 'T', 'C'};
	int ret;

	pr_debug("%s: WDT(%d) Close\n", __func__, data->wdev_idx);
	ret = nct6694_write_msg(nct6694, REQUEST_WDT_MOD,
				REQUEST_WDT_CMD1_OFFSET(data->wdev_idx),
				REQUEST_WDT_CMD1_LEN, buf);
	if (ret)
		pr_err("%s: Failed to start WDT device!\n", __func__);

	return ret;
}

static int nct6694_wdt_ping(struct watchdog_device *wdev)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694 *nct6694 = data->nct6694;
	unsigned char buf[REQUEST_WDT_CMD1_LEN] = {'W', 'D', 'T', 'S'};
	int ret;

	pr_debug("%s: WDT(%d) Ping\n", __func__, data->wdev_idx);
	ret = nct6694_write_msg(nct6694, REQUEST_WDT_MOD,
				REQUEST_WDT_CMD1_OFFSET(data->wdev_idx),
				REQUEST_WDT_CMD1_LEN, buf);
	if (ret)
		pr_err("%s: Failed to ping WDT device!\n", __func__);

	return ret;
}

static int nct6694_wdt_set_timeout(struct watchdog_device *wdev,
				   unsigned int timeout)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694 *nct6694 = data->nct6694;
	unsigned int timeout_fmt, pretimeout_fmt;
	unsigned char buf[REQUEST_WDT_CMD0_LEN];
	int ret;

	if (timeout < wdev->pretimeout) {
		pr_err("%s: 'timeout' must be greater than 'pre timeout'!\n",
		       __func__);
		return -EINVAL;
	}

	timeout_fmt = timeout * 1000 | (WDT_TIMEOUT_ACT << 24);
	pretimeout_fmt = wdev->pretimeout * 1000 | (WDT_PRETIMEOUT_ACT << 24);
	set_buf32(&buf[WDT_TIMEOUT_IDX], le32_to_cpu(timeout_fmt));
	set_buf32(&buf[WDT_PRETIMEOUT_IDX], le32_to_cpu(pretimeout_fmt));

	ret = nct6694_write_msg(nct6694, REQUEST_WDT_MOD,
				REQUEST_WDT_CMD0_OFFSET(data->wdev_idx),
				REQUEST_WDT_CMD0_LEN, buf);
	if (ret) {
		pr_err("%s: Don't write the setup command in Start stage!\n",
		       __func__);
		return ret;
	}

	wdev->timeout = timeout;

	return 0;
}

static int nct6694_wdt_set_pretimeout(struct watchdog_device *wdev,
				      unsigned int pretimeout)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694 *nct6694 = data->nct6694;
	unsigned int timeout_fmt, pretimeout_fmt;
	unsigned char buf[REQUEST_WDT_CMD0_LEN];
	int ret;

	if (pretimeout > wdev->timeout) {
		pr_err("%s: 'pre timeout' must be less than 'timeout'!\n",
		       __func__);
		return -EINVAL;
	}
	timeout_fmt = wdev->timeout * 1000 | (WDT_TIMEOUT_ACT << 24);
	pretimeout_fmt = pretimeout * 1000 | (WDT_PRETIMEOUT_ACT << 24);
	set_buf32(&buf[WDT_TIMEOUT_IDX], le32_to_cpu(timeout_fmt));
	set_buf32(&buf[WDT_PRETIMEOUT_IDX], le32_to_cpu(pretimeout_fmt));

	ret = nct6694_write_msg(nct6694, REQUEST_WDT_MOD,
				REQUEST_WDT_CMD0_OFFSET(data->wdev_idx),
				REQUEST_WDT_CMD0_LEN, buf);
	if (ret) {
		pr_err("%s: Don't write the setup command in Start stage!\n", __func__);
		return ret;
	}

	wdev->pretimeout = pretimeout;
	return 0;
}

static unsigned int nct6694_wdt_get_time(struct watchdog_device *wdev)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694 *nct6694 = data->nct6694;
	unsigned char buf[WDT_COUNTDOWN_LEN];
	unsigned int timeleft_ms;
	int ret;

	ret = nct6694_read_msg(nct6694, REQUEST_WDT_MOD,
			       REQUEST_WDT_CMD0_OFFSET(data->wdev_idx),
			       REQUEST_WDT_CMD0_LEN, WDT_COUNTDOWN_IDX,
			       WDT_COUNTDOWN_LEN, buf);
	if (ret)
		pr_err("%s: Failed to get WDT device!\n", __func__);

	timeleft_ms = ((buf[2] << 16) | (buf[1] << 8) | buf[0]) & 0xFFFFFF;

	return timeleft_ms / 1000;
}

static int nct6694_wdt_setup(struct watchdog_device *wdev)
{
	struct nct6694_wdt_data *data = watchdog_get_drvdata(wdev);
	struct nct6694 *nct6694 = data->nct6694;
	unsigned char buf[REQUEST_WDT_CMD0_LEN] = {0};
	unsigned int timeout_fmt, pretimeout_fmt;
	int ret;

	if (timeout)
		wdev->timeout = timeout;

	if (pretimeout) {
		wdev->pretimeout = pretimeout;
		pretimeout_fmt = wdev->pretimeout * 1000 | (WDT_PRETIMEOUT_ACT << 24);
	} else {
		pretimeout_fmt = 0;
	}

	timeout_fmt = wdev->timeout * 1000 | (WDT_TIMEOUT_ACT << 24);
	set_buf32(&buf[WDT_TIMEOUT_IDX], le32_to_cpu(timeout_fmt));
	set_buf32(&buf[WDT_PRETIMEOUT_IDX], le32_to_cpu(pretimeout_fmt));

	ret = nct6694_write_msg(nct6694, REQUEST_WDT_MOD,
				REQUEST_WDT_CMD0_OFFSET(data->wdev_idx),
				REQUEST_WDT_CMD0_LEN, buf);
	if (ret)
		return ret;

	pr_info("Setting WDT(%d): timeout = %d, pretimeout = %d\n",
		data->wdev_idx, wdev->timeout, wdev->pretimeout);

	return 0;
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
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	struct nct6694_wdt_data *data;
	struct watchdog_device *wdev;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->nct6694 = nct6694;
	data->wdev_idx = cell->id;

	wdev = &data->wdev;
	wdev->info = &nct6694_wdt_info;
	wdev->ops = &nct6694_wdt_ops;
	wdev->timeout = WATCHDOG_TIMEOUT;
	wdev->pretimeout = WATCHDOG_PRETIMEOUT;
	wdev->min_timeout = 1;
	wdev->max_timeout = 255;

	platform_set_drvdata(pdev, data);

	/* Register watchdog timer device to WDT framework */
	watchdog_set_drvdata(&data->wdev, data);
	watchdog_init_timeout(&data->wdev, timeout, &pdev->dev);
	watchdog_set_nowayout(&data->wdev, nowayout);
	watchdog_stop_on_reboot(&data->wdev);

	ret = devm_watchdog_register_device(&pdev->dev, &data->wdev);
	if (ret) {
		dev_err(&pdev->dev, "%s: Failed to register watchdog device: %d\n",
			__func__, ret);
		return ret;
	}

	ret = nct6694_wdt_setup(&data->wdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to setup WDT device!\n");
		return ret;
	}

	return 0;
}

static struct platform_driver nct6694_wdt_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_wdt_probe,
};

static int __init nct6694_init(void)
{
	int err;

	err = platform_driver_register(&nct6694_wdt_driver);
	if (!err) {
		if (err)
			platform_driver_unregister(&nct6694_wdt_driver);
	}

	return err;
}
subsys_initcall(nct6694_init);

static void __exit nct6694_exit(void)
{
	platform_driver_unregister(&nct6694_wdt_driver);
}
module_exit(nct6694_exit);

MODULE_DESCRIPTION("USB-WDT driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
