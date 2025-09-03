// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/gunyah_errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define GUNYAH_WDT_SMCCC_CALL_VAL(func_id) \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32,\
			   ARM_SMCCC_OWNER_VENDOR_HYP, func_id)

/* SMCCC function IDs for watchdog operations */
#define GUNYAH_WDT_CONTROL   GUNYAH_WDT_SMCCC_CALL_VAL(0x0005)
#define GUNYAH_WDT_STATUS    GUNYAH_WDT_SMCCC_CALL_VAL(0x0006)
#define GUNYAH_WDT_PING       GUNYAH_WDT_SMCCC_CALL_VAL(0x0007)
#define GUNYAH_WDT_SET_TIME  GUNYAH_WDT_SMCCC_CALL_VAL(0x0008)

/*
 * Control values for GUNYAH_WDT_CONTROL.
 * Bit 0 is used to enable or disable the watchdog. If this bit is set,
 * then the watchdog is enabled and vice versa.
 * Bit 1 should always be set to 1 as this bit is reserved in Gunyah and
 * it's expected to be 1.
 */
#define WDT_CTRL_ENABLE  (BIT(1) | BIT(0))
#define WDT_CTRL_DISABLE BIT(1)

struct gunyah_wdt {
	unsigned int pretimeout_irq;
	struct watchdog_device wdd;
};

static int gunyah_wdt_call(unsigned long func_id, unsigned long arg1,
			   unsigned long arg2, struct arm_smccc_res *res)
{
	arm_smccc_1_1_smc(func_id, arg1, arg2, res);
	return gunyah_error_remap(res->a0);
}

static int gunyah_wdt_start(struct watchdog_device *wdd)
{
	struct arm_smccc_res res;
	unsigned int timeout_ms;
	unsigned int pretimeout_ms;
	int ret;

	ret = gunyah_wdt_call(GUNYAH_WDT_CONTROL, WDT_CTRL_DISABLE, 0, &res);
	if (ret)
		return ret;

	timeout_ms = wdd->timeout * 1000;
	pretimeout_ms = wdd->pretimeout * 1000;
	ret = gunyah_wdt_call(GUNYAH_WDT_SET_TIME,
			      pretimeout_ms, timeout_ms, &res);
	if (ret)
		return ret;

	return gunyah_wdt_call(GUNYAH_WDT_CONTROL, WDT_CTRL_ENABLE, 0, &res);
}

static int gunyah_wdt_stop(struct watchdog_device *wdd)
{
	struct arm_smccc_res res;

	return gunyah_wdt_call(GUNYAH_WDT_CONTROL, WDT_CTRL_DISABLE, 0, &res);
}

static int gunyah_wdt_ping(struct watchdog_device *wdd)
{
	struct arm_smccc_res res;

	return gunyah_wdt_call(GUNYAH_WDT_PING, 0, 0, &res);
}

static int gunyah_wdt_set_timeout(struct watchdog_device *wdd,
		     unsigned int timeout_sec)
{
	wdd->timeout = timeout_sec;

	if (watchdog_active(wdd))
		return gunyah_wdt_start(wdd);

	return 0;
}

static int gunyah_wdt_set_pretimeout(struct watchdog_device *wdd,
				     unsigned int pretimeout_sec)
{
	wdd->pretimeout = pretimeout_sec;

	if (watchdog_active(wdd))
		return gunyah_wdt_start(wdd);

	return 0;
}

static unsigned int gunyah_wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct arm_smccc_res res;
	unsigned int seconds_since_last_ping;
	int ret;

	ret = gunyah_wdt_call(GUNYAH_WDT_STATUS, 0, 0, &res);
	if (ret)
		return 0;

	seconds_since_last_ping = res.a2 / 1000;
	if (seconds_since_last_ping > wdd->timeout)
		return 0;

	return wdd->timeout - seconds_since_last_ping;
}

static int gunyah_wdt_restart(struct watchdog_device *wdd,
			      unsigned long action, void *data)
{
	struct arm_smccc_res res;

	/* Set timeout and pretimeout to 1ms and send a ping */
	gunyah_wdt_call(GUNYAH_WDT_CONTROL, WDT_CTRL_ENABLE, 0, &res);
	gunyah_wdt_call(GUNYAH_WDT_SET_TIME, 1, 1, &res);
	gunyah_wdt_call(GUNYAH_WDT_PING, 0, 0, &res);

	/* Wait to make sure reset occurs */
	mdelay(100);

	return 0;
}

static const struct watchdog_info gunyah_wdt_info = {
	.identity = "Gunyah Watchdog",
	.firmware_version = 0,
	.options = WDIOF_SETTIMEOUT
		 | WDIOF_PRETIMEOUT
		 | WDIOF_KEEPALIVEPING
		 | WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops gunyah_wdt_ops = {
	.owner = THIS_MODULE,
	.start = gunyah_wdt_start,
	.stop = gunyah_wdt_stop,
	.ping = gunyah_wdt_ping,
	.set_timeout = gunyah_wdt_set_timeout,
	.set_pretimeout = gunyah_wdt_set_pretimeout,
	.get_timeleft = gunyah_wdt_get_timeleft,
	.restart = gunyah_wdt_restart
};

static irqreturn_t gunyah_wdt_pretimeout_handler(int irq, void *arg)
{
	struct watchdog_device *wdd = arg;

	watchdog_notify_pretimeout(wdd);

	return IRQ_HANDLED;
}

static int gunyah_wdt_probe(struct platform_device *pdev)
{
	struct gunyah_wdt *wdt;
	struct device *dev = &pdev->dev;
	int ret;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->wdd.info = &gunyah_wdt_info;
	wdt->wdd.ops = &gunyah_wdt_ops;
	wdt->wdd.parent = dev;

	/*
	 * Although Gunyah expects 16-bit unsigned int values as timeout values
	 * in milliseconds, values above 0x8000 are reserved. This limits the
	 * max timeout value to 32 seconds.
	 */
	wdt->wdd.max_timeout = 32; /* seconds */
	wdt->wdd.min_timeout = 1; /* seconds */
	wdt->wdd.timeout = wdt->wdd.max_timeout;
	wdt->wdd.pretimeout = wdt->wdd.timeout - 2;

	gunyah_wdt_stop(&wdt->wdd);
	watchdog_init_timeout(&wdt->wdd, 0, dev);

	platform_set_drvdata(pdev, wdt);

	watchdog_set_restart_priority(&wdt->wdd, 0);
	ret = devm_watchdog_register_device(dev, &wdt->wdd);
	if (ret) {
		dev_err(dev, "Failed to register watchdog device: %d\n", ret);
		return ret;
	}

	/*
	 * Register the pretimeout irq as rising edge triggered irrespective of
	 * the irqflags passed by Gunyah to make the driver compatible with
	 * pretimeout governors like noop.
	 */
	wdt->pretimeout_irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(dev, wdt->pretimeout_irq,
			       gunyah_wdt_pretimeout_handler,
			       IRQF_TRIGGER_RISING,
			       "wdt_pretimeout", &wdt->wdd);
	if (ret) {
		dev_err(dev, "Failed to register pretimeout irq: %d\n", ret);
		return ret;
	}

	dev_dbg(dev, "Gunyah watchdog registered\n");
	return 0;
}

static int __maybe_unused gunyah_wdt_suspend(struct device *dev)
{
	struct gunyah_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_active(&wdt->wdd))
		gunyah_wdt_stop(&wdt->wdd);

	return 0;
}

static int __maybe_unused gunyah_wdt_resume(struct device *dev)
{
	struct gunyah_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_active(&wdt->wdd))
		gunyah_wdt_start(&wdt->wdd);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(gunyah_wdt_pm_ops, gunyah_wdt_suspend, gunyah_wdt_resume);

static const struct of_device_id gunyah_wdt_of_match[] = {
	{ .compatible = "qcom,gh-watchdog" },
	{ }
};
MODULE_DEVICE_TABLE(of, gunyah_wdt_of_match);

static struct platform_driver gunyah_wdt_driver = {
	.probe = gunyah_wdt_probe,
	.driver = {
		.name = "gunyah-wdt",
		.of_match_table = gunyah_wdt_of_match,
		.pm = pm_sleep_ptr(&gunyah_wdt_pm_ops),
	},
};

static int __init gunyah_wdt_init(void)
{
	return platform_driver_register(&gunyah_wdt_driver);
}

module_init(gunyah_wdt_init);

MODULE_DESCRIPTION("Gunyah Watchdog Driver");
MODULE_LICENSE("GPL");
