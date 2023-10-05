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
#define SUPPORT_PWRBTN		BIT(3)
#define SUPPORT_IRQ		BIT(4)
#define SUPPORT_SCI		BIT(5)
#define SUPPORT_PIN		BIT(6)
#define SUPPORT_RESET		BIT(7)

/* PMC registers */
#define REG_STATUS		0x00
#define REG_CONTROL		0x02
#define REG_EVENT		0x10
#define REG_PWR_EVENT_TIME	0x12
#define REG_IRQ_EVENT_TIME	0x13
#define REG_RESET_EVENT_TIME	0x14
#define REG_PIN_EVENT_TIME	0x15
#define REG_SCI_EVENT_TIME	0x16
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

/* Mapping event type to supported bit */
#define EVENT_BIT(type)   	BIT(type + 2)

enum event_type {
	EVENT_NONE,
	EVENT_PWRBTN,
	EVENT_IRQ,
	EVENT_SCI,
	EVENT_PIN
};

static struct _wdt {
	u32	event_type;
	u32	support;
	long	last_time;
	struct	regmap  *iomap;
	struct	device *dev;
} wdt;

static char * const type_strs[] = {
	"NONE",
	"PWRBTN",
	"IRQ",
	"SCI",
	"PIN",
};

static u32 type_regs[] = {
	REG_RESET_EVENT_TIME,
	REG_PWR_EVENT_TIME,
	REG_IRQ_EVENT_TIME,
	REG_SCI_EVENT_TIME,
	REG_PIN_EVENT_TIME,
};

/* Pointer to the eiois200_core device structure */
static struct eiois200_dev *eiois200_dev;

/* Specify the pin triggered on pretimeout or timeout */
static char *event_type = "NONE";
module_param(event_type, charp, 0);
MODULE_PARM_DESC(event_type,
		 "Watchdog timeout event type (RESET, PWRBTN, SCI, IRQ, GPIO)");
static struct watchdog_info wdinfo = {
	.identity = KBUILD_MODNAME,
	.options  = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
		    WDIOF_PRETIMEOUT | WDIOF_MAGICCLOSE,
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
	dev_info(wdt.dev, "Set timeout: %d\n", _timeout);

	return 0;
}

static int wdt_set_pretimeout(struct watchdog_device *dev,
			      unsigned int _pretimeout)
{
	dev->pretimeout = _pretimeout;

	dev_info(wdt.dev, "Set pretimeout: %d\n", _pretimeout);

	return 0;
}

static int wdt_get_type(void)
{
	int i;

	for (i = 1; i < ARRAY_SIZE(type_strs); i++)
		if (strcasecmp(event_type, type_strs[i]) == 0) {
			if ((wdt.support & EVENT_BIT(i)) == 0) {
				dev_err(wdt.dev,
					"This board doesn't support %s trigger type\n",
					event_type);
				return -EINVAL;
			}

			dev_info(wdt.dev, "Trigger type is %d:%s\n", 
					  i, type_strs[i]);
			wdt.event_type = i;

			return 0;
		}

	dev_info(wdt.dev, "Event type: %s\n", type_strs[wdt.event_type]);
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
	int ret, type;
	u32 event_time = 0;
	u32 reset_time = 0;

	/* event_type should never out of range */
	if (wdt.event_type > EVENT_PIN)
		return -EFAULT;

	/* Calculate event time and reset time */
	if (wddev.pretimeout && wddev.timeout) {
		if (wddev.timeout < wddev.pretimeout)
			return -EINVAL;

	reset_time = wddev.timeout;
		event_time = wddev.timeout - wddev.pretimeout;

	} else if (wddev.timeout) {
		reset_time = wdt.event_type ? 0	: wddev.timeout;
		event_time = wdt.event_type ? wddev.timeout : 0;
	}

	/* Set reset time */
	ret = set_time(REG_RESET_EVENT_TIME, reset_time);
	if (ret)
		return ret;

	/* Set every other times */
	for (type = 1; type < ARRAY_SIZE(type_regs); type++) {
		ret = set_time(type_regs[type],
			       wdt.event_type == type ? event_time : 0);
		if (ret)
			return ret;
	}

	dev_dbg(wdt.dev, "Config wdt reset time %d\n", reset_time);
	dev_dbg(wdt.dev, "Config wdt event time %d\n", event_time);
	dev_dbg(wdt.dev, "Config wdt event type %s\n",
			  type_strs[wdt.event_type]);

	return ret;
}

static int wdt_get_config(void)
{
	int ret, type;
	u32 event_time, reset_time;

	/* Get Reset Time */
	ret = get_time(REG_RESET_EVENT_TIME, &reset_time);
	if (ret)
		return ret;

	dev_dbg(wdt.dev, "Timeout H/W default timeout: %d secs\n", reset_time);

	/* Get every other times **/
	for (type = 1; type < ARRAY_SIZE(type_regs); type++) {
		if ((wdt.support & EVENT_BIT(type)) == 0)
			continue;

		ret = get_time(type_regs[type], &event_time);
		if (ret)
			return ret;

		if (event_time == 0)
			continue;

		if (reset_time) {
			if (reset_time < event_time)
				continue;

	wddev.timeout	 = reset_time;
			wddev.pretimeout = reset_time - event_time;

			dev_dbg(wdt.dev, "Pretimeout H/W enabled with event %s of %d secs\n",
				 type_strs[type], wddev.pretimeout);
		} else {
			wddev.timeout = event_time;
			wddev.pretimeout = 0;
		}

		wdt.event_type = type;

		dev_dbg(wdt.dev, "Timeout H/W enabled of %d secs\n",
				  wddev.timeout);
		return 0;
	}

	wdt.event_type	 = EVENT_NONE;
	wddev.pretimeout = reset_time ? 0	   : WATCHDOG_PRETIMEOUT;
	wddev.timeout	 = reset_time ? reset_time : WATCHDOG_TIMEOUT;

	dev_dbg(wdt.dev, "Pretimeout H/W disabled");
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

	ret = wdt_get_type();
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
	.set_pretimeout = wdt_set_pretimeout,
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
