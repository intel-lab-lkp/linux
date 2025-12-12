// SPDX-License-Identifier: GPL-2.0-only
/*
 * Advantech EIO Watchdog Driver
 *
 * Copyright (C) 2025 Advantech Co., Ltd.
 */

#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>
#include <linux/mfd/eio.h>

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

/* Mapping event type to supported bit */
#define EVENT_BIT(type)	BIT(type + 2)

enum event_type {
	EVENT_NONE,
	EVENT_PWRBTN,
	EVENT_IRQ,
	EVENT_SCI,
	EVENT_PIN
};

struct eio_wdt_dev {
	u32 event_type;
	u32 support;
	int irq;
	unsigned long last_time;
	struct regmap *iomap;
	struct device *mfd;
	struct device *dev;
	struct watchdog_device wdd;
	struct eio_dev *core;
};

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

/* Specify the pin triggered on pretimeout or timeout */
static char *event_type = "NONE";
module_param(event_type, charp, 0);
MODULE_PARM_DESC(event_type, "Watchdog timeout event type (NONE, PWRBTN, IRQ, SCI, PIN)");

/* Specify the IRQ number when the IRQ event is triggered */
static int irq;
module_param(irq, int, 0);
MODULE_PARM_DESC(irq, "The IRQ number for IRQ event");

static int timeout;
module_param(timeout, int, 0444);
MODULE_PARM_DESC(timeout, "Set PMC command timeout value.\n");

static int pmc_write(struct device *dev, u8 ctrl, void *data)
{
	struct pmc_op op = {
		.cmd       = CMD_WDT_WRITE,
		.control   = ctrl,
		.payload   = data,
		.size     = (ctrl <= REG_EVENT) ? 1 :
			    (ctrl >= REG_IRQ_NUMBER) ? 1 : 4,
		.timeout   = timeout,
	};
	return eio_core_pmc_operation(dev, &op);
}

static int pmc_read(struct device *dev, u8 ctrl, void *data)
{
	struct pmc_op op = {
		.cmd       = CMD_WDT_READ,
		.control   = ctrl,
		.payload   = data,
		.size     = (ctrl <= REG_EVENT) ? 1 :
			    (ctrl >= REG_IRQ_NUMBER) ? 1 : 4,
		.timeout   = timeout,
	};
	return eio_core_pmc_operation(dev, &op);
}

static int wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
	struct eio_wdt_dev *eio_wdt = watchdog_get_drvdata(wdd);

	wdd->timeout = timeout;
	dev_info(eio_wdt->dev, "Set timeout: %u\n", timeout);

	return 0;
}

static int wdt_set_pretimeout(struct watchdog_device *wdd, unsigned int pretimeout)
{
	struct eio_wdt_dev *eio_wdt = watchdog_get_drvdata(wdd);

	wdd->pretimeout = pretimeout;
	dev_info(eio_wdt->dev, "Set pretimeout: %u\n", pretimeout);

	return 0;
}

static int wdt_get_type(struct eio_wdt_dev *eio_wdt)
{
	int i;

	for (i = 1; i < ARRAY_SIZE(type_strs); i++) {
		if (strcasecmp(event_type, type_strs[i]) == 0) {
			if ((eio_wdt->support & EVENT_BIT(i)) == 0) {
				dev_err(eio_wdt->dev,
					"This board doesn't support %s trigger type\n",
					event_type);
				return -EINVAL;
			}

			dev_info(eio_wdt->dev, "Trigger type is %d:%s\n",
				 i, type_strs[i]);
			eio_wdt->event_type = i;
			return 0;
		}
	}

	dev_info(eio_wdt->dev, "Event type: %s\n",
		 type_strs[eio_wdt->event_type]);
	return 0;
}

static int get_time(struct eio_wdt_dev *eio_wdt, u8 ctrl, u32 *val)
{
	int ret;

	ret = pmc_read(eio_wdt->mfd, ctrl, val);
	if (ret)
		return ret;

	/* ms to sec */
	*val /= 1000;

	return 0;
}

static int set_time(struct eio_wdt_dev *eio_wdt, u8 ctrl, u32 time)
{
	/* sec to ms */
	time *= 1000;

	return pmc_write(eio_wdt->mfd, ctrl, &time);
}

static int wdt_set_config(struct eio_wdt_dev *eio_wdt)
{
	int ret, type;
	u32 event_time = 0;
	u32 reset_time = 0;

	if (eio_wdt->event_type > EVENT_PIN)
		return -EFAULT;

	/* Calculate event time and reset time */
	if (eio_wdt->wdd.pretimeout && eio_wdt->wdd.timeout) {
		if (eio_wdt->wdd.timeout < eio_wdt->wdd.pretimeout)
			return -EINVAL;

		reset_time = eio_wdt->wdd.timeout;
		event_time = eio_wdt->wdd.timeout - eio_wdt->wdd.pretimeout;

	} else if (eio_wdt->wdd.timeout) {
		reset_time = eio_wdt->event_type ?	0 : eio_wdt->wdd.timeout;
		event_time = eio_wdt->event_type ? eio_wdt->wdd.timeout : 0;
	}

	/* Set reset time */
	ret = set_time(eio_wdt, REG_RESET_EVENT_TIME, reset_time);
	if (ret)
		return ret;

	/* Set every other times */
	for (type = 1; type < ARRAY_SIZE(type_regs); type++) {
		ret = set_time(eio_wdt, type_regs[type],
			       (eio_wdt->event_type == type) ? event_time : 0);
		if (ret)
			return ret;
	}

	dev_dbg(eio_wdt->dev, "Config wdt reset time %u\n", reset_time);
	dev_dbg(eio_wdt->dev, "Config wdt event time %u\n", event_time);
	dev_dbg(eio_wdt->dev, "Config wdt event type %s\n",
		type_strs[eio_wdt->event_type]);

	return 0;
}

static int wdt_get_config(struct eio_wdt_dev *eio_wdt)
{
	int ret, type;
	u32 event_time = 0, reset_time = 0;

	/* Get Reset Time */
	ret = get_time(eio_wdt, REG_RESET_EVENT_TIME, &reset_time);
	if (ret)
		return ret;

	dev_dbg(eio_wdt->dev, "Timeout H/W default timeout: %u secs\n", reset_time);

	/* Get every other times */
	for (type = 1; type < ARRAY_SIZE(type_regs); type++) {
		if ((eio_wdt->support & EVENT_BIT(type)) == 0)
			continue;

		ret = get_time(eio_wdt, type_regs[type], &event_time);
		if (ret)
			return ret;

		if (event_time == 0)
			continue;

		if (reset_time) {
			if (reset_time < event_time)
				continue;

			eio_wdt->wdd.timeout    = reset_time;
			eio_wdt->wdd.pretimeout = reset_time - event_time;

			dev_dbg(eio_wdt->dev,
				"Pretimeout H/W enabled with event %s of %u secs\n",
				type_strs[type], eio_wdt->wdd.pretimeout);
		} else {
			eio_wdt->wdd.timeout    = event_time;
			eio_wdt->wdd.pretimeout = 0;
		}

		eio_wdt->event_type = type;

		dev_dbg(eio_wdt->dev, "Timeout H/W enabled of %u secs\n",
			eio_wdt->wdd.timeout);
		return 0;
	}

	eio_wdt->event_type         = EVENT_NONE;
	eio_wdt->wdd.pretimeout     = reset_time ? 0 : WATCHDOG_PRETIMEOUT;
	eio_wdt->wdd.timeout        = reset_time ? reset_time : WATCHDOG_TIMEOUT;

	dev_dbg(eio_wdt->dev, "Pretimeout H/W disabled\n");
	return 0;
}

static int set_ctrl(struct eio_wdt_dev *eio_wdt, u8 ctrl)
{
	return pmc_write(eio_wdt->mfd, REG_CONTROL, &ctrl);
}

static int wdt_start(struct watchdog_device *wdd)
{
	struct eio_wdt_dev *eio_wdt = watchdog_get_drvdata(wdd);
	int ret;

	ret = wdt_set_config(eio_wdt);
	if (ret)
		return ret;

	ret = set_ctrl(eio_wdt, CTRL_START);
	if (!ret) {
		eio_wdt->last_time = jiffies;
		dev_dbg(eio_wdt->dev, "Watchdog started\n");
	}

	return ret;
}

static int wdt_stop(struct watchdog_device *wdd)
{
	struct eio_wdt_dev *eio_wdt = watchdog_get_drvdata(wdd);
	int ret;

	dev_dbg(eio_wdt->dev, "Watchdog stopped\n");
	eio_wdt->last_time = 0;

	ret = set_ctrl(eio_wdt, CTRL_STOP);
	return ret;
}

static int wdt_ping(struct watchdog_device *wdd)
{
	struct eio_wdt_dev *eio_wdt = watchdog_get_drvdata(wdd);
	int ret;

	dev_dbg(eio_wdt->dev, "Watchdog ping\n");

	ret = set_ctrl(eio_wdt, CTRL_TRIGGER);
	if (!ret)
		eio_wdt->last_time = jiffies;

	return ret;
}

static unsigned int wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct eio_wdt_dev *eio_wdt = watchdog_get_drvdata(wdd);
	unsigned int timeleft = 0;

	if (eio_wdt->last_time && wdd->timeout) {
		unsigned long delta   = jiffies - eio_wdt->last_time;
		unsigned int  elapsed = (unsigned int)(delta / HZ);

		if (elapsed < wdd->timeout)
			timeleft = wdd->timeout - elapsed;
	}
	return timeleft;
}

static int wdt_support(struct eio_wdt_dev *eio_wdt)
{
	u8 support;

	if (pmc_read(eio_wdt->mfd, REG_STATUS, &support))
		return -EIO;

	if (!(support & SUPPORT_AVAILABLE))
		return -ENODEV;

	if ((support & SUPPORT_RESET) != SUPPORT_RESET)
		return -ENODEV;

	eio_wdt->support = support;

	return 0;
}

static int wdt_get_irq_io(struct eio_wdt_dev *eio_wdt)
{
	int ret  = 0;
	int idx  = EIO_PNP_INDEX;
	int data = EIO_PNP_DATA;
	struct regmap *map = eio_wdt->iomap;

	mutex_lock(&eio_wdt->core->mutex);

	/* Unlock EC IO port */
	ret |= regmap_write(map, idx, IOREG_UNLOCK);
	ret |= regmap_write(map, idx, IOREG_UNLOCK);

	/* Select logical device to PMC */
	ret |= regmap_write(map, idx,  IOREG_LDN);
	ret |= regmap_write(map, data, IOREG_LDN_PMCIO);

	/* Get IRQ number */
	ret |= regmap_write(map, idx,  IOREG_IRQ);
	ret |= regmap_read(map, data, &eio_wdt->irq);

	/* Lock back */
	ret |= regmap_write(map, idx, IOREG_LOCK);

	mutex_unlock(&eio_wdt->core->mutex);

	return ret ? -EIO : 0;
}

static int wdt_get_irq_pmc(struct eio_wdt_dev *eio_wdt)
{
	return pmc_read(eio_wdt->mfd, REG_IRQ_NUMBER, &eio_wdt->irq);
}

static int wdt_get_irq(struct eio_wdt_dev *eio_wdt)
{
	int ret;

	if (!(eio_wdt->support & BIT(EVENT_IRQ)))
		return -ENODEV;

	ret = wdt_get_irq_pmc(eio_wdt);
	if (ret) {
		dev_err(eio_wdt->dev, "Error get irq by pmc\n");
		return ret;
	}

	if (eio_wdt->irq)
		return 0;

	/* Fallback: get IRQ number from EC IO space */
	ret = wdt_get_irq_io(eio_wdt);
	if (ret) {
		dev_err(eio_wdt->dev, "Error get irq by io\n");
		return ret;
	}

	if (!eio_wdt->irq) {
		dev_err(eio_wdt->dev, "Error IRQ number = 0\n");
		return -EIO;
	}

	return 0;
}

static int wdt_set_irq_io(struct eio_wdt_dev *eio_wdt)
{
	int ret  = 0;
	int idx  = EIO_PNP_INDEX;
	int data = EIO_PNP_DATA;
	struct regmap *map = eio_wdt->iomap;

	mutex_lock(&eio_wdt->core->mutex);

	/* Unlock EC IO port */
	ret |= regmap_write(map, idx, IOREG_UNLOCK);
	ret |= regmap_write(map, idx, IOREG_UNLOCK);

	/* Select logical device to PMC */
	ret |= regmap_write(map, idx,  IOREG_LDN);
	ret |= regmap_write(map, data, IOREG_LDN_PMCIO);

	/* Enable WDT */
	ret |= regmap_write(map, idx,  IOREG_WDT_STATUS);
	ret |= regmap_write(map, data, FLAG_WDT_ENABLED);

	/* Set IRQ number */
	ret |= regmap_write(map, idx,  IOREG_IRQ);
	ret |= regmap_write(map, data, eio_wdt->irq);

	/* Lock back */
	ret |= regmap_write(map, idx, IOREG_LOCK);

	mutex_unlock(&eio_wdt->core->mutex);

	return ret ? -EIO : 0;
}

static int wdt_set_irq_pmc(struct eio_wdt_dev *eio_wdt)
{
	return pmc_write(eio_wdt->mfd, REG_IRQ_NUMBER, &eio_wdt->irq);
}

static int wdt_set_irq(struct eio_wdt_dev *eio_wdt)
{
	int ret;

	if (!(eio_wdt->support & BIT(EVENT_IRQ)))
		return -ENODEV;

	ret = wdt_set_irq_io(eio_wdt);
	if (ret) {
		dev_err(eio_wdt->dev, "Error set irq by io\n");
		return ret;
	}

	ret = wdt_set_irq_pmc(eio_wdt);
	if (ret) {
		dev_err(eio_wdt->dev, "Error set irq by pmc\n");
		return ret;
	}

	return 0;
}

static int wdt_get_irq_event(struct eio_wdt_dev *eio_wdt)
{
	u8 status;

	if (pmc_read(eio_wdt->mfd, REG_EVENT, &status))
		return 0;

	return status;
}

static irqreturn_t wdt_isr(int irq, void *arg)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t wdt_threaded_isr(int irq, void *arg)
{
	struct eio_wdt_dev *eio_wdt = arg;
	u8 status = wdt_get_irq_event(eio_wdt) & FLAG_TRIGGER_IRQ;

	if (!status)
		return IRQ_NONE;

	if (eio_wdt->wdd.pretimeout) {
		watchdog_notify_pretimeout(&eio_wdt->wdd);
	} else {
		dev_crit(eio_wdt->dev, "Watchdog expired, rebooting\n");
		emergency_restart();
	}

	return IRQ_HANDLED;
}

static int query_irq(struct eio_wdt_dev *eio_wdt)
{
	int ret = 0;

	if (irq) {
		eio_wdt->irq = irq;
	} else {
		ret = wdt_get_irq(eio_wdt);
		if (ret)
			return ret;
	}

	dev_dbg(eio_wdt->dev, "IRQ = %d\n", eio_wdt->irq);

	return wdt_set_irq(eio_wdt);
}

static int wdt_init(struct eio_wdt_dev *eio_wdt)
{
	int ret;

	ret = wdt_support(eio_wdt);
	if (ret)
		return ret;

	ret = wdt_get_config(eio_wdt);
	if (ret)
		return ret;

	ret = wdt_get_type(eio_wdt);
	if (ret)
		return ret;

	if (eio_wdt->event_type == EVENT_IRQ)
		ret = query_irq(eio_wdt);

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

static struct watchdog_info wdinfo = {
	.identity = KBUILD_MODNAME,
	.options  = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
		    WDIOF_PRETIMEOUT | WDIOF_MAGICCLOSE,
};

static int eio_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct eio_wdt_dev *eio_wdt;
	struct watchdog_device *wdd;
	int ret = 0;

	eio_wdt = devm_kzalloc(dev, sizeof(*eio_wdt), GFP_KERNEL);
	if (!eio_wdt)
		return -ENOMEM;

	eio_wdt->dev = dev;
	eio_wdt->mfd = dev->parent;
	eio_wdt->iomap = dev_get_regmap(dev->parent, NULL);
	if (!eio_wdt->iomap)
		return dev_err_probe(dev, -ENODEV, "parent regmap missing\n");

	eio_wdt->core = dev_get_drvdata(dev->parent);
	if (!eio_wdt->core)
		return dev_err_probe(dev, -ENODEV, "eio_core not present\n");

	ret = wdt_init(eio_wdt);
	if (ret) {
		dev_err(dev, "wdt_init fail\n");
		return -EIO;
	}

	if (eio_wdt->event_type == EVENT_IRQ) {
		ret = devm_request_threaded_irq(dev, eio_wdt->irq,
						wdt_isr, wdt_threaded_isr,
						IRQF_SHARED | IRQF_ONESHOT, pdev->name,
						eio_wdt);
		if (ret) {
			dev_err(dev, "IRQ %d request fail:%d. Disabled.\n",
				eio_wdt->irq, ret);
			return ret;
		}
	}

	wdd = &eio_wdt->wdd;
	wdd->info        = &wdinfo;
	wdd->ops         = &wdt_ops;
	wdd->parent      = dev;
	wdd->min_timeout = 1;
	wdd->max_timeout = 0x7FFF;

	ret = watchdog_init_timeout(wdd, wdd->timeout, dev);
	if (ret) {
		dev_err(dev, "Init timeout fail\n");
		return ret;
	}

	watchdog_stop_on_reboot(&eio_wdt->wdd);
	watchdog_stop_on_unregister(&eio_wdt->wdd);

	watchdog_set_drvdata(&eio_wdt->wdd, eio_wdt);
	platform_set_drvdata(pdev, eio_wdt);

	ret = devm_watchdog_register_device(dev, &eio_wdt->wdd);
	if (ret)
		dev_err(dev, "Cannot register watchdog device (err: %d)\n", ret);

	return ret;
}

static struct platform_driver eio_wdt_driver = {
	.probe  = eio_wdt_probe,
	.driver = {
		.name = "eio_wdt",
	},
};
module_platform_driver(eio_wdt_driver);

MODULE_AUTHOR("Wenkai Chung <wenkai.chung@advantech.com.tw>");
MODULE_AUTHOR("Ramiro Oliveira <ramiro.oliveira@advantech.com>");
MODULE_DESCRIPTION("Watchdog interface for Advantech EIO embedded controller");
MODULE_LICENSE("GPL");
