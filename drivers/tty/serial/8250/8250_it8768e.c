// SPDX-License-Identifier: GPL-2.0+
/*
 * Probe for 8250/16550-type iTE IT8768E serial ports.
 *
 * Based on drivers/char/serial.c which is in the history, by Linus Torvalds, Theodore Ts'o.
 *
 * Copyright (C) 2025 Uniontech Ltd.
 * Author: WangYuli <wangyuli@uniontech.com>
 */
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pnp.h>
#include <linux/serial_core.h>
#include <linux/string.h>

#include <asm/byteorder.h>

#include "8250.h"

struct it8768e_data {
	struct uart_8250_port uart;
	int line;
};

static int it8768e_probe(struct platform_device *pdev)
{
	struct it8768e_data *data;
	struct resource *res;
	void *__iomem sio_base;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "memory resource not found\n");
		return -EINVAL;
	}

	sio_base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!sio_base) {
		dev_err(&pdev->dev, "devm_ioremap error\n");
		return -ENOMEM;
	}

	data = devm_kcalloc(&pdev->dev, 1,
			sizeof(struct it8768e_data),
			GFP_KERNEL);
	if (!data) {
		dev_err(&pdev->dev, "Failed to alloc private mem struct.\n");
		return -ENOMEM;
	}

	spin_lock_init(&data->uart.port.lock);
	data->uart.port.dev = &pdev->dev;
	data->uart.port.regshift = 0;
	data->uart.port.iotype = UPIO_MEM;
	data->uart.port.type = PORT_16550A;
	data->uart.port.membase = sio_base;
	data->uart.port.mapbase = res->start;
	data->uart.port.uartclk = 1843200;
	data->uart.port.flags = UPF_FIXED_PORT | UPF_FIXED_TYPE | UPF_SKIP_TEST;

	data->line = serial8250_register_8250_port(&data->uart);
	if (data->line < 0) {
		dev_err(&pdev->dev,
			"unable to resigter 8250 port (MEM%llx): %d\n",
			(unsigned long long)res->start, 0);
		return data->line;
	}

	dev_set_drvdata(&pdev->dev, data);
	return 0;
}

static void it8768e_remove(struct platform_device *pdev)
{
	struct it8768e_data *data = dev_get_drvdata(&pdev->dev);

	if (!data)
		return;

	del_timer(&data->uart.timer);
	serial8250_unregister_port(data->line);
}

#ifdef CONFIG_PM
static int it8768e_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct it8768e_data *data = dev_get_drvdata(&pdev->dev);

	if (!data)
		return -ENODEV;

	serial8250_suspend_port(data->line);
	return 0;
}

static int it8768e_resume(struct platform_device *pdev)
{
	struct it8768e_data *data = dev_get_drvdata(&pdev->dev);

	if (!data)
		return -ENODEV;

	serial8250_resume_port(data->line);

	return 0;
}
#else
#define it8768e_suspend NULL
#define it8768e_resume NULL
#endif /* PM */

#ifdef CONFIG_ACPI
static const struct acpi_device_id it8768e_acpi_ids[] = {
	{ .id = "ITES0001" },
	{}
};
MODULE_DEVICE_TABLE(acpi, it8768e_acpi_ids);
#else
#define it8768e_acpi_ids NULL
#endif /* ACPI */

static struct platform_driver it8768e_driver = {
	.probe	= it8768e_probe,
	.remove = it8768e_remove,
	.suspend = it8768e_suspend,
	.resume	 = it8768e_resume,
	.driver = {
		.name = "it8768e",
		.acpi_match_table = it8768e_acpi_ids,
	},
};
module_platform_driver(it8768e_driver);

MODULE_AUTHOR("WangYuli");
MODULE_DESCRIPTION("8250 uart driver for iTE IT8768E");
MODULE_LICENSE("GPL");
