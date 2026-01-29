// SPDX-License-Identifier: GPL-2.0+
/*
 * Watchdog device driver for Sony Cronos SMCs
 * Copyright (C) 2015 Dialog Semiconductor Ltd.
 * Copyright (C) 2022-2025 Raptor Engineering, LLC
 *
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mfd/sony-cronos.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>

static const unsigned int wdt_timeout[] = { 10, 80 };
static const unsigned int wdt_timeout_ctl_bits[] = { 0x1, 0x0 };
#define CRONOS_TWDSCALE_DISABLE 0
#define CRONOS_TWDSCALE_MIN 1
#define CRONOS_TWDSCALE_MAX (ARRAY_SIZE(wdt_timeout) - 1)
#define CRONOS_WDT_MIN_TIMEOUT wdt_timeout[CRONOS_TWDSCALE_MIN]
#define CRONOS_WDT_MAX_TIMEOUT wdt_timeout[CRONOS_TWDSCALE_MAX]
#define CRONOS_WDG_DEFAULT_TIMEOUT wdt_timeout[CRONOS_TWDSCALE_MAX]

struct sony_cronos_watchdog {
	struct sony_cronos_smc *hw;
	struct watchdog_device wdtdev;
};

static unsigned int sony_cronos_wdt_read_timeout(struct sony_cronos_watchdog *wdt)
{
	unsigned int i;
	unsigned int val;

	regmap_read(wdt->hw->regmap, CRONOS_WDT_CTL_REG, &val);

	for (i = CRONOS_TWDSCALE_MIN; i <= CRONOS_TWDSCALE_MAX; i++) {
		if (wdt_timeout_ctl_bits[i] == (val & CRONOS_WDT_TIMEOUT_MASK))
			return wdt_timeout[i];
	}

	dev_err(wdt->hw->dev, "Invalid configuration data present in watchdog control register!\n");
	return wdt_timeout[CRONOS_WDT_MIN_TIMEOUT];
}

static unsigned int sony_cronos_wdt_timeout_to_sel(unsigned int secs)
{
	unsigned int i;

	for (i = CRONOS_TWDSCALE_MIN; i <= CRONOS_TWDSCALE_MAX; i++) {
		if (wdt_timeout[i] >= secs)
			return wdt_timeout_ctl_bits[i];
	}

	return wdt_timeout_ctl_bits[CRONOS_TWDSCALE_MAX];
}

static int sony_cronos_reset_watchdog_timer(struct sony_cronos_watchdog *wdt)
{
	return regmap_write(wdt->hw->regmap, CRONOS_WDT_CLR_REG, CRONOS_WDT_CLR_VAL);
}

static int sony_cronos_wdt_update_timeout_register(struct sony_cronos_watchdog *wdt,
						   unsigned int regval)
{
	int ret;

	struct sony_cronos_smc *chip = wdt->hw;

	ret = sony_cronos_reset_watchdog_timer(wdt);
	if (ret) {
		dev_err(wdt->hw->dev, "Watchdog failed to reset (err = %d)\n", ret);
		goto done;
	}

	return regmap_update_bits(chip->regmap, CRONOS_WDT_CTL_REG, CRONOS_WDT_TIMEOUT_MASK,
				  regval);

done:
	return ret;
}

static int sony_cronos_wdt_start(struct watchdog_device *wdd)
{
	struct sony_cronos_watchdog *wdt = watchdog_get_drvdata(wdd);
	struct sony_cronos_smc *chip = wdt->hw;
	unsigned int selector;
	int ret;

	selector = sony_cronos_wdt_timeout_to_sel(wdt->wdtdev.timeout);
	ret = sony_cronos_wdt_update_timeout_register(wdt, selector);
	if (ret) {
		dev_err(wdt->hw->dev, "Watchdog prestart configuration failed (err = %d)\n", ret);
		goto done;
	}

	ret = regmap_update_bits(chip->regmap, CRONOS_WDT_CTL_REG, CRONOS_WDT_ENABLE_MASK, 1);

	if (ret)
		dev_err(wdt->hw->dev, "Watchdog failed to start (err = %d)\n", ret);

done:
	return ret;
}

static int sony_cronos_wdt_stop(struct watchdog_device *wdd)
{
	struct sony_cronos_watchdog *wdt = watchdog_get_drvdata(wdd);
	struct sony_cronos_smc *chip = wdt->hw;
	int ret;

	ret = regmap_update_bits(chip->regmap, CRONOS_WDT_CTL_REG, CRONOS_WDT_ENABLE_MASK, 1);
	if (ret)
		dev_err(wdt->hw->dev, "Watchdog failed to stop (err = %d)\n", ret);

	return ret;
}

static int sony_cronos_wdt_ping(struct watchdog_device *wdd)
{
	struct sony_cronos_watchdog *wdt = watchdog_get_drvdata(wdd);
	int ret;

	/*
	 * Prevent pings from occurring late in system poweroff/reboot sequence
	 * and possibly locking out restart handler from accessing i2c bus.
	 */
	if (system_state > SYSTEM_RUNNING)
		return 0;

	ret = sony_cronos_reset_watchdog_timer(wdt);
	if (ret)
		dev_err(wdt->hw->dev, "Failed to ping the watchdog (err = %d)\n", ret);

	return ret;
}

static int sony_cronos_wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
	struct sony_cronos_watchdog *wdt = watchdog_get_drvdata(wdd);
	unsigned int selector;
	int ret;

	selector = sony_cronos_wdt_timeout_to_sel(timeout);
	ret = sony_cronos_wdt_update_timeout_register(wdt, selector);
	if (ret)
		dev_err(wdt->hw->dev, "Failed to set watchdog timeout (err = %d)\n", ret);
	else
		wdd->timeout = wdt_timeout[selector];

	return ret;
}

static int sony_cronos_wdt_restart(struct watchdog_device *wdd, unsigned long action, void *data)
{
	struct sony_cronos_watchdog *wdt = watchdog_get_drvdata(wdd);
	struct i2c_client *client = to_i2c_client(wdt->hw->dev);
	int ret;

	/* Don't use regmap because it is not atomic safe */
	ret = i2c_smbus_write_byte_data(client, CRONOS_WDT_CTL_REG, CRONOS_WDT_CTL_RESET_VAL);
	ret = i2c_smbus_write_byte_data(client, CRONOS_BMC_RESET_REG, CRONOS_BMC_RESET_VAL);
	if (ret < 0)
		dev_alert(wdt->hw->dev, "Failed to shutdown (err = %d)\n", ret);

	/* wait for reset to assert... */
	mdelay(500);

	return ret;
}

static const struct watchdog_info sony_cronos_watchdog_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
	.identity = "Sony Cronos WDT",
};

static const struct watchdog_ops sony_cronos_watchdog_ops = {
	.owner = THIS_MODULE,
	.start = sony_cronos_wdt_start,
	.stop = sony_cronos_wdt_stop,
	.ping = sony_cronos_wdt_ping,
	.set_timeout = sony_cronos_wdt_set_timeout,
	.restart = sony_cronos_wdt_restart,
};

static const struct of_device_id sony_cronos_compatible_id_table[] = {
	{
		.compatible = "sony,cronos-watchdog",
	},
	{},
};

MODULE_DEVICE_TABLE(of, sony_cronos_compatible_id_table);

static int sony_cronos_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int timeout;
	struct sony_cronos_smc *chip;
	struct sony_cronos_watchdog *wdt;

	chip = dev_get_drvdata(dev->parent);
	if (!chip)
		return -EINVAL;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->hw = chip;

	wdt->wdtdev.info = &sony_cronos_watchdog_info;
	wdt->wdtdev.ops = &sony_cronos_watchdog_ops;
	wdt->wdtdev.min_timeout = CRONOS_WDT_MIN_TIMEOUT;
	wdt->wdtdev.max_timeout = CRONOS_WDT_MAX_TIMEOUT;
	wdt->wdtdev.min_hw_heartbeat_ms = 0;
	wdt->wdtdev.timeout = CRONOS_WDG_DEFAULT_TIMEOUT;
	wdt->wdtdev.status = WATCHDOG_NOWAYOUT_INIT_STATUS;
	wdt->wdtdev.parent = dev;

	watchdog_set_restart_priority(&wdt->wdtdev, 128);

	watchdog_set_drvdata(&wdt->wdtdev, wdt);
	dev_set_drvdata(dev, &wdt->wdtdev);

	timeout = sony_cronos_wdt_read_timeout(wdt);
	if (timeout)
		wdt->wdtdev.timeout = timeout;

	/* Set timeout from DT value if available */
	watchdog_init_timeout(&wdt->wdtdev, 0, dev->parent);

	if (timeout) {
		sony_cronos_wdt_set_timeout(&wdt->wdtdev, wdt->wdtdev.timeout);
		set_bit(WDOG_HW_RUNNING, &wdt->wdtdev.status);
	}

	return devm_watchdog_register_device(dev, &wdt->wdtdev);
}

static int __maybe_unused sony_cronos_wdt_suspend(struct device *dev)
{
	struct watchdog_device *wdd = dev_get_drvdata(dev);

	if (watchdog_active(wdd))
		return sony_cronos_wdt_stop(wdd);

	return 0;
}

static int __maybe_unused sony_cronos_wdt_resume(struct device *dev)
{
	struct watchdog_device *wdd = dev_get_drvdata(dev);

	if (watchdog_active(wdd))
		return sony_cronos_wdt_start(wdd);

	return 0;
}

static SIMPLE_DEV_PM_OPS(sony_cronos_wdt_pm_ops, sony_cronos_wdt_suspend, sony_cronos_wdt_resume);

static struct platform_driver sony_cronos_wdt_driver = {
	.probe = sony_cronos_wdt_probe,
	.driver = {
		.name = "sony-cronos-watchdog",
		.pm = &sony_cronos_wdt_pm_ops,
		.of_match_table = sony_cronos_compatible_id_table,
	},
};
module_platform_driver(sony_cronos_wdt_driver);

MODULE_AUTHOR("Raptor Engineering, LLC <tpearson@raptorengineering.com>");
MODULE_DESCRIPTION("WDT device driver for Sony Cronos SMCs");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sony-cronos-watchdog");
