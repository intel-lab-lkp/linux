// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define GUNYAH_WDT_DRV_NAME "gunyah-wdt"

#define GUNYAH_WDT_SMCCC_CALL_VAL(func_id) \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32,\
			   ARM_SMCCC_OWNER_VENDOR_HYP, func_id)

/* SMCCC function IDs for watchdog operations */
#define GUNYAH_WDT_CONTROL   GUNYAH_WDT_SMCCC_CALL_VAL(0x0005)
#define GUNYAH_WDT_STATUS    GUNYAH_WDT_SMCCC_CALL_VAL(0x0006)
#define GUNYAH_WDT_PING      GUNYAH_WDT_SMCCC_CALL_VAL(0x0007)
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

enum gunyah_error {
	GUNYAH_ERROR_OK				= 0,
	GUNYAH_ERROR_UNIMPLEMENTED		= -1,
	GUNYAH_ERROR_ARG_INVAL			= 1,
};

static struct platform_device *gunyah_wdt_dev;

/**
 * gunyah_error_remap() - Remap Gunyah hypervisor errors into a Linux error code
 * @gunyah_error: Gunyah hypercall return value
 */
static inline int gunyah_error_remap(enum gunyah_error gunyah_error)
{
	switch (gunyah_error) {
	case GUNYAH_ERROR_OK:
		return 0;
	case GUNYAH_ERROR_UNIMPLEMENTED:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}

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
	struct device *dev = wdd->parent;
	int ret;

	ret = gunyah_wdt_call(GUNYAH_WDT_CONTROL, WDT_CTRL_DISABLE, 0, &res);
	if (ret && watchdog_active(wdd)) {
		dev_err(dev, "%s: Failed to stop gunyah wdt %d\n", __func__, ret);
		return ret;
	}

	timeout_ms = wdd->timeout * 1000;
	ret = gunyah_wdt_call(GUNYAH_WDT_SET_TIME,
			      timeout_ms, timeout_ms, &res);
	if (ret) {
		dev_err(dev, "%s: Failed to set timeout for gunyah wdt %d\n",
			__func__, ret);
		return ret;
	}

	ret = gunyah_wdt_call(GUNYAH_WDT_CONTROL, WDT_CTRL_ENABLE, 0, &res);
	if (ret)
		dev_err(dev, "%s: Failed to start gunyah wdt %d\n", __func__, ret);

	return ret;
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

	/* Set timeout to 1ms and send a ping */
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
		 | WDIOF_KEEPALIVEPING
		 | WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops gunyah_wdt_ops = {
	.owner = THIS_MODULE,
	.start = gunyah_wdt_start,
	.stop = gunyah_wdt_stop,
	.ping = gunyah_wdt_ping,
	.set_timeout = gunyah_wdt_set_timeout,
	.get_timeleft = gunyah_wdt_get_timeleft,
	.restart = gunyah_wdt_restart
};

static int gunyah_wdt_probe(struct platform_device *pdev)
{
	struct watchdog_device *wdd;
	struct device *dev = &pdev->dev;
	int ret;

	wdd = devm_kzalloc(dev, sizeof(*wdd), GFP_KERNEL);
	if (!wdd)
		return -ENOMEM;

	wdd->info = &gunyah_wdt_info;
	wdd->ops = &gunyah_wdt_ops;
	wdd->parent = dev;

	/*
	 * Although Gunyah expects 16-bit unsigned int values as timeout values
	 * in milliseconds, values above 0x8000 are reserved. This limits the
	 * max timeout value to 32 seconds.
	 */
	wdd->max_timeout = 32; /* seconds */
	wdd->min_timeout = 1; /* seconds */
	wdd->timeout = wdd->max_timeout;

	gunyah_wdt_stop(wdd);
	platform_set_drvdata(pdev, wdd);
	watchdog_set_restart_priority(wdd, 0);

	ret = devm_watchdog_register_device(dev, wdd);
	if (ret) {
		dev_err(dev, "Failed to register watchdog device: %d\n", ret);
		return ret;
	}

	dev_dbg(dev, "Gunyah watchdog registered\n");
	return 0;
}

static int __maybe_unused gunyah_wdt_suspend(struct device *dev)
{
	struct watchdog_device *wdd = dev_get_drvdata(dev);

	if (watchdog_active(wdd))
		gunyah_wdt_stop(wdd);

	return 0;
}

static int __maybe_unused gunyah_wdt_resume(struct device *dev)
{
	struct watchdog_device *wdd = dev_get_drvdata(dev);

	if (watchdog_active(wdd))
		gunyah_wdt_start(wdd);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(gunyah_wdt_pm_ops, gunyah_wdt_suspend, gunyah_wdt_resume);

static struct platform_driver gunyah_wdt_driver = {
	.probe = gunyah_wdt_probe,
	.driver = {
		.name = GUNYAH_WDT_DRV_NAME,
		.pm = pm_sleep_ptr(&gunyah_wdt_pm_ops),
	},
};

static int __init gunyah_wdt_init(void)
{
	struct arm_smccc_res res;
	struct device_node *np;
	int ret;

	/* Check if we're running on a Qualcomm device */
	np = of_find_compatible_node(NULL, NULL, "qcom,smem");
	if (!np)
		return -ENODEV;
	of_node_put(np);

	/*
	 * When Gunyah is not present or Gunyah is emulating a memory-mapped
	 * watchdog, either of Qualcomm watchdog or ARM SBSA watchdog will be
	 * present. Skip initialization of SMC-based Gunyah watchdog if that is
	 * the case.
	 */
	np = of_find_compatible_node(NULL, NULL, "qcom,kpss-wdt");
	if (np) {
		of_node_put(np);
		return -ENODEV;
	}

	np = of_find_compatible_node(NULL, NULL, "arm,sbsa-gwdt");
	if (np) {
		of_node_put(np);
		return -ENODEV;
	}

	ret = gunyah_wdt_call(GUNYAH_WDT_STATUS, 0, 0, &res);
	if (ret)
		return -ENODEV;

	ret = platform_driver_register(&gunyah_wdt_driver);
	if (ret)
		return ret;

	gunyah_wdt_dev = platform_device_register_simple(GUNYAH_WDT_DRV_NAME,
							 -1, NULL, 0);
	if (IS_ERR(gunyah_wdt_dev)) {
		platform_driver_unregister(&gunyah_wdt_driver);
		return PTR_ERR(gunyah_wdt_dev);
	}

	return 0;
}

module_init(gunyah_wdt_init);

MODULE_DESCRIPTION("Gunyah Watchdog Driver");
MODULE_LICENSE("GPL");
