// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Aaeon MCU Watchdog driver
 *
 * Copyright (C) 2025 Bootlin
 * Author: Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>
 * Author: Thomas Perrot <thomas.perrot@bootlin.com>
 */

#include <linux/i2c.h>
#include <linux/mfd/aaeon-mcu.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define AAEON_MCU_CONTROL_WDT 0x63
#define AAEON_MCU_PING_WDT 0x73

#define AAEON_MCU_WDT_TIMEOUT         240
#define AAEON_MCU_WDT_HEARTBEAT_MS    25000

struct aaeon_mcu_wdt {
	struct watchdog_device wdt;
	struct aaeon_mcu_dev *mfd;
};

static int aaeon_mcu_wdt_start_cmd(struct aaeon_mcu_wdt *data)
{
	u8 cmd[3], rsp;

	cmd[0] = AAEON_MCU_CONTROL_WDT;
	cmd[1] = 0x01;
	cmd[2] = 0x00;

	return aaeon_mcu_i2c_xfer(data->mfd->i2c_client, cmd, 3, &rsp, 1);
}

static int aaeon_mcu_wdt_start(struct watchdog_device *wdt)
{
	struct aaeon_mcu_wdt *data = watchdog_get_drvdata(wdt);

	return aaeon_mcu_wdt_start_cmd(data);
}

static int aaeon_mcu_wdt_stop_cmd(struct aaeon_mcu_wdt *data)
{
	u8 cmd[3], rsp;

	cmd[0] = AAEON_MCU_CONTROL_WDT;
	cmd[1] = 0x00;
	cmd[2] = 0x00;

	return aaeon_mcu_i2c_xfer(data->mfd->i2c_client, cmd, 3, &rsp, 1);
}

static int aaeon_mcu_wdt_stop(struct watchdog_device *wdt)
{
	struct aaeon_mcu_wdt *data = watchdog_get_drvdata(wdt);

	return aaeon_mcu_wdt_stop_cmd(data);
}

static int aaeon_mcu_wdt_ping_cmd(struct aaeon_mcu_wdt *data)
{
	u8 cmd[3], rsp;

	cmd[0] = AAEON_MCU_PING_WDT;
	cmd[1] = 0x00;
	cmd[2] = 0x00;

	return aaeon_mcu_i2c_xfer(data->mfd->i2c_client, cmd, 3, &rsp, 1);
}

static int aaeon_mcu_wdt_ping(struct watchdog_device *wdt)
{
	struct aaeon_mcu_wdt *data = watchdog_get_drvdata(wdt);

	return aaeon_mcu_wdt_ping_cmd(data);
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
	struct aaeon_mcu_dev *mcu = dev_get_drvdata(dev->parent);
	struct watchdog_device *wdt;
	struct aaeon_mcu_wdt *data;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->mfd = mcu;

	wdt = &data->wdt;
	wdt->parent = dev;

	wdt->info = &aaeon_mcu_wdt_info;
	wdt->ops = &aaeon_mcu_wdt_ops;
	wdt->max_hw_heartbeat_ms = AAEON_MCU_WDT_HEARTBEAT_MS;
	watchdog_init_timeout(wdt, AAEON_MCU_WDT_TIMEOUT, dev);

	watchdog_set_drvdata(wdt, data);
	platform_set_drvdata(pdev, data);
	set_bit(WDOG_HW_RUNNING, &wdt->status);

	return devm_watchdog_register_device(dev, wdt);
}

static const struct of_device_id aaeon_mcu_wdt_of_match[] = {
	{ .compatible = "aaeon,srg-imx8pl-wdt" },
	{},
};

MODULE_DEVICE_TABLE(of, aaeon_mcu_wdt_of_match);

static struct platform_driver aaeon_mcu_wdt_driver = {
	.driver		= {
		.name	= "aaeon-mcu-wdt",
		.of_match_table = aaeon_mcu_wdt_of_match,
	},
	.probe		= aaeon_mcu_wdt_probe,
};

module_platform_driver(aaeon_mcu_wdt_driver);

MODULE_DESCRIPTION("Aaeon MCU Watchdog Driver");
MODULE_AUTHOR("Jérémie Dautheribes");
MODULE_LICENSE("GPL");
