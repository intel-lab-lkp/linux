// SPDX-License-Identifier: GPL-2.0-only
/*
 * Advantech EIO-IS200 Watchdog Driver
 *
 * Copyright (C) 2023 Advantech Co., Ltd.
 * Author: wenkai <advantech.susiteam@gmail.com>
 */

#include <linux/mfd/core.h>
#include <linux/reboot.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>

#include "../mfd/eiois200.h"

#define WATCHDOG_TIMEOUT	60
#define WATCHDOG_PRETIMEOUT	10

/* Support Flags */
#define SUPPORT_AVAILABLE	BIT(0)
#define SUPPORT_RESET		BIT(7)

/* PMC registers */
#define REG_STATUS		0x00
#define REG_CONTROL		0x02
#define REG_EVENT		0x10
#define REG_RESET_EVENT_TIME	0x14
#define REG_IRQ_NUMBER		0x17

/* PMC command and control */
#define CMD_WDT_WRITE		0x2A
#define CMD_WDT_READ		0x2B
#define CTRL_STOP		0x00
#define CTRL_START		0x01
#define CTRL_TRIGGER		0x02

/* I/O register and its flags */
#define IOREG_UNLOCK		0x87
#define IOREG_LOCK		0xAA
#define IOREG_LDN		0x07
#define IOREG_LDN_PMCIO		0x0F
#define IOREG_IRQ		0x70
#define IOREG_WDT_STATUS	0x30

/* Flags */
#define FLAG_WDT_ENABLED	0x01
#define FLAG_TRIGGER_IRQ	BIT(4)

/* PMC read and write a value */
#define PMC_WRITE(cmd, data)	pmc(CMD_WDT_WRITE, cmd, data)
#define PMC_READ(cmd, data)	pmc(CMD_WDT_READ, cmd, data)

static struct _wdt {
	u32	support;
	long	last_time;
	struct	regmap  *iomap;
	struct	device *dev;
} wdt;

/* Pointer to the eiois200_core device structure */
static struct eiois200_dev *eiois200_dev;

static struct watchdog_info wdinfo = {
	.identity = KBUILD_MODNAME,
	.options  = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
		    WDIOF_MAGICCLOSE,
};

static struct watchdog_device wddev = {
	.info	     = &wdinfo,
	.max_timeout = 0x7FFF,
	.min_timeout = 1,
};

static int wdt_set_timeout(struct watchdog_device *dev,
			   unsigned int _timeout)
{
	dev->timeout = _timeout;
	dev_dbg(wdt.dev, "Set timeout: %d\n", _timeout);

	return 0;
}

static int pmc(u8 cmd, u8 ctrl, void *payload)
{
	struct pmc_op op = {
		.cmd      = cmd,
		.control  = ctrl,
		.size     = ctrl <= REG_EVENT	   ? 1 :
			    ctrl >= REG_IRQ_NUMBER ? 1 : 4,
		.payload  = payload,
	};

	return eiois200_core_pmc_operation(wdt.dev, &op);
}

static int get_time(u8 ctrl, u32 *val)
{
	int ret;

	ret = PMC_READ(ctrl, val);

	/* ms to sec */
	*val /= 1000;

	return ret;
}

static int set_time(u8 ctl, u32 time)
{
	/* sec to sec */
	time *= 1000;

	return PMC_WRITE(ctl, &time);
}

static int wdt_set_config(void)
{
	int ret;
	u32 reset_time = 0;

	reset_time = wddev.timeout;

	ret = set_time(REG_RESET_EVENT_TIME, reset_time);
	if (ret)
		return ret;

	dev_info(wdt.dev, "Config wdt reset time %d\n", reset_time);

	return ret;
}

static int wdt_get_config(void)
{
	int ret;
	u32 reset_time;

	/* Get Reset Time */
	ret = get_time(REG_RESET_EVENT_TIME, &reset_time);
	if (ret)
		return ret;

	dev_info(wdt.dev, "Timeout H/W default timeout: %d secs\n", reset_time);
	wddev.timeout	 = reset_time;

	return 0;
}

static int set_ctrl(u8 data)
{
	return PMC_WRITE(REG_CONTROL, &data);
}

static int wdt_start(struct watchdog_device *dev)
{
	int ret;

	ret = wdt_set_config();
	if (ret)
		return ret;

	ret = set_ctrl(CTRL_START);
	if (ret == 0) {
		wdt.last_time = jiffies;
		dev_dbg(wdt.dev, "Watchdog started\n");
	}

	return ret;
}

static int wdt_stop(struct watchdog_device *dev)
{
	dev_dbg(wdt.dev, "Watchdog stopped\n");
	wdt.last_time = 0;

	return set_ctrl(CTRL_STOP);
}

static int wdt_ping(struct watchdog_device *dev)
{
	int ret;

	dev_dbg(wdt.dev, "Watchdog pings\n");

	ret = set_ctrl(CTRL_TRIGGER);
	if (ret == 0)
		wdt.last_time = jiffies;

	return ret;
}

static unsigned int wdt_get_timeleft(struct watchdog_device *dev)
{
	unsigned int timeleft = 0;

	if (wdt.last_time != 0)
		timeleft = wddev.timeout - ((jiffies - wdt.last_time) / HZ);

	return timeleft;
}

static int wdt_support(void)
{
	u8 support;

	if (PMC_READ(REG_STATUS, &support))
		return -EIO;

	if ((support & SUPPORT_AVAILABLE) == 0)
		return -EIO;

	/* Must support reset */
	if ((support & SUPPORT_RESET) != SUPPORT_RESET)
		return -EIO;

	/* Must has support event **/
	wdt.support = support;

	return 0;
}
static int wdt_init(struct device *dev)
{
	int ret = 0;

	ret = wdt_support();
	if (ret)
		return ret;

	ret = wdt_get_config();
	if (ret)
		return ret;

	return ret;
}

static const struct watchdog_ops wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= wdt_start,
	.stop		= wdt_stop,
	.ping		= wdt_ping,
	.set_timeout	= wdt_set_timeout,
	.get_timeleft	= wdt_get_timeleft,
};

static int wdt_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;

	/* Contact eiois200_core */
	eiois200_dev = dev_get_drvdata(dev->parent);
	if (!eiois200_dev)
		return dev_err_probe(dev, ret,
				     "Error contact eiois200_core %d\n", ret);

	wdt.dev = dev;
	wdt.iomap = dev_get_regmap(dev->parent, NULL);
	if (!wdt.iomap)
		return dev_err_probe(dev, -ENOMEM, "Query parent regmap fail\n");
	
	/* Initialize EC watchdog */
	if (wdt_init(dev))
		return dev_err_probe(dev, -EIO, "wdt_init fail\n");

	/* Inform watchdog info */
	wddev.ops = &wdt_ops;
	ret = watchdog_init_timeout(&wddev, wddev.timeout, dev);
	if (ret)
		return dev_err_probe(dev, ret, "Init timeout fail\n");

	watchdog_stop_on_reboot(&wddev);

	watchdog_stop_on_unregister(&wddev);

	/* Register watchdog */
	ret = devm_watchdog_register_device(dev, &wddev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Cannot register watchdog device (err: %d)\n",
				     ret);

	return 0;
}

static struct platform_driver eiois200_wdt_driver = {
	.driver = {
		.name  = "eiois200_wdt",
	},
};
module_platform_driver_probe(eiois200_wdt_driver, wdt_probe);

MODULE_AUTHOR("wenkai <advantech.susiteam@gmail.com>");
MODULE_DESCRIPTION("Watchdog interface for Advantech EIO-IS200 embedded controller");
MODULE_LICENSE("GPL");
