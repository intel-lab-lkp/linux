// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Aaeon MCU Watchdog driver
 *
 * Copyright (C) 2025 Bootlin
 * Author: Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>
 * Author: Thomas Perrot <thomas.perrot@bootlin.com>
 */

#include <linux/mfd/aaeon-mcu.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/watchdog.h>

#define AAEON_MCU_CONTROL_WDT	0x63
#define AAEON_MCU_PING_WDT	0x73

#define AAEON_MCU_WDT_TIMEOUT         240
#define AAEON_MCU_WDT_HEARTBEAT_MS    25000

struct aaeon_mcu_wdt {
	struct watchdog_device wdt;
	struct regmap *regmap;
};

static int aaeon_mcu_wdt_cmd(struct aaeon_mcu_wdt *data, u8 opcode, u8 arg)
{
	/* The MCU always sends a response byte after each command; discard it. */
	return regmap_write(data->regmap, AAEON_MCU_REG(opcode, arg), 0);
}

static int aaeon_mcu_wdt_start(struct watchdog_device *wdt)
{
	struct aaeon_mcu_wdt *data = watchdog_get_drvdata(wdt);

	return aaeon_mcu_wdt_cmd(data, AAEON_MCU_CONTROL_WDT, 0x01);
}

static int aaeon_mcu_wdt_status(struct watchdog_device *wdt, bool *enabled)
{
	struct aaeon_mcu_wdt *data = watchdog_get_drvdata(wdt);
	unsigned int rsp;
	int ret;

	ret = regmap_read(data->regmap,
			  AAEON_MCU_REG(AAEON_MCU_CONTROL_WDT, 0x02),
			  &rsp);
	if (ret)
		return ret;

	*enabled = rsp == 0x01;
	return 0;
}

static int aaeon_mcu_wdt_stop(struct watchdog_device *wdt)
{
	struct aaeon_mcu_wdt *data = watchdog_get_drvdata(wdt);

	return aaeon_mcu_wdt_cmd(data, AAEON_MCU_CONTROL_WDT, 0x00);
}

static int aaeon_mcu_wdt_ping(struct watchdog_device *wdt)
{
	struct aaeon_mcu_wdt *data = watchdog_get_drvdata(wdt);

	return aaeon_mcu_wdt_cmd(data, AAEON_MCU_PING_WDT, 0x00);
}

static const struct watchdog_info aaeon_mcu_wdt_info = {
	.identity	= "Aaeon MCU Watchdog",
	.options	= WDIOF_KEEPALIVEPING
};

static const struct watchdog_ops aaeon_mcu_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= aaeon_mcu_wdt_start,
	.stop		= aaeon_mcu_wdt_stop,
	.ping		= aaeon_mcu_wdt_ping,
};

static int aaeon_mcu_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct watchdog_device *wdt;
	struct aaeon_mcu_wdt *data;
	bool enabled;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regmap = dev_get_regmap(dev->parent, NULL);
	if (!data->regmap)
		return -ENODEV;

	wdt = &data->wdt;
	wdt->parent = dev;
	wdt->info = &aaeon_mcu_wdt_info;
	wdt->ops = &aaeon_mcu_wdt_ops;
	/*
	 * The MCU firmware has a fixed hardware timeout of 25 seconds that
	 * cannot be changed. The watchdog core will handle automatic pinging
	 * to support longer timeouts. The software timeout of 240 seconds is
	 * chosen arbitrarily as a reasonable value and is not user-configurable.
	 */
	wdt->timeout = AAEON_MCU_WDT_TIMEOUT;
	wdt->max_hw_heartbeat_ms = AAEON_MCU_WDT_HEARTBEAT_MS;

	watchdog_set_drvdata(wdt, data);

	ret = aaeon_mcu_wdt_status(wdt, &enabled);
	if (ret)
		return ret;

	if (enabled)
		set_bit(WDOG_HW_RUNNING, &wdt->status);

	return devm_watchdog_register_device(dev, wdt);
}

static struct platform_driver aaeon_mcu_wdt_driver = {
	.driver		= {
		.name	= "aaeon-mcu-wdt",
	},
	.probe		= aaeon_mcu_wdt_probe,
};

module_platform_driver(aaeon_mcu_wdt_driver);

MODULE_DESCRIPTION("Aaeon MCU Watchdog Driver");
MODULE_AUTHOR("Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>");
MODULE_LICENSE("GPL");
