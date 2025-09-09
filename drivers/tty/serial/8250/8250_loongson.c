// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Serial Port driver for Loongson family chips
 *
 * Copyright (C) 2020-2025 Loongson Technology Corporation Limited
 */

#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/reset.h>

#include "8250.h"

/* Divisor Latch Fraction Register */
#define LOONGSON_UART_DLF		0x2

/* Flags */
#define LOONGSON_UART_HAS_FRAC		BIT(0)
#define LOONGSON_UART_QUIRK_MCR		BIT(1)
#define LOONGSON_UART_QUIRK_MSR		BIT(2)

#define LS2K0500_UART_FLAG	(LOONGSON_UART_QUIRK_MCR | LOONGSON_UART_QUIRK_MSR)
#define LS2K1500_UART_FLAG	(LOONGSON_UART_HAS_FRAC | LOONGSON_UART_QUIRK_MCR)

struct loongson_uart_data {
	int line;
	int mcr_invert;
	int msr_invert;
	struct reset_control *rst;
};

static unsigned int serial_fixup(struct uart_port *p, unsigned int offset, unsigned int val)
{
	struct loongson_uart_data *ddata = p->private_data;

	if (offset == UART_MCR)
		val ^= ddata->mcr_invert;

	if (offset == UART_MSR)
		val ^= ddata->msr_invert;

	return val;
}

static u32 loongson_serial_in(struct uart_port *p, unsigned int offset)
{
	unsigned int val;

	val = readb(p->membase + (offset << p->regshift));

	return serial_fixup(p, offset, val);
}

static void loongson_serial_out(struct uart_port *p, unsigned int offset, unsigned int value)
{
	offset <<= p->regshift;
	writeb(serial_fixup(p, offset, value), p->membase + offset);
}

static unsigned int loongson_frac_get_divisor(struct uart_port *port, unsigned int baud,
					      unsigned int *frac)
{
	unsigned int quot;

	quot = DIV_ROUND_CLOSEST((port->uartclk << 4), baud);
	*frac = FIELD_GET(GENMASK(7, 0), quot);

	return FIELD_GET(GENMASK(15, 8), quot);
}

static void loongson_frac_set_divisor(struct uart_port *port, unsigned int baud,
				      unsigned int quot, unsigned int quot_frac)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	serial_port_out(port, UART_LCR, up->lcr | UART_LCR_DLAB);
	serial_dl_write(up, quot);
	serial_port_out(port, LOONGSON_UART_DLF, quot_frac);
}

static int loongson_uart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uart_8250_port uart = {};
	struct loongson_uart_data *ddata;
	struct resource *res;
	unsigned int flags;
	int ret;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	uart.port.irq = platform_get_irq(pdev, 0);
	if (uart.port.irq < 0)
		return -EINVAL;

	device_property_read_u32(dev, "clock-frequency", &uart.port.uartclk);

	spin_lock_init(&uart.port.lock);
	uart.port.flags = UPF_SHARE_IRQ | UPF_FIXED_PORT | UPF_FIXED_TYPE | UPF_IOREMAP;
	uart.port.iotype = UPIO_MEM;
	uart.port.regshift = 0;
	uart.port.dev = dev;
	uart.port.type = PORT_LOONGSON;
	uart.port.private_data = ddata;

	uart.port.mapbase = res->start;
	uart.port.mapsize = resource_size(res);
	uart.port.serial_in = loongson_serial_in;
	uart.port.serial_out = loongson_serial_out;

	flags = (uintptr_t)device_get_match_data(dev);

	if (flags & LOONGSON_UART_HAS_FRAC) {
		uart.port.get_divisor = loongson_frac_get_divisor;
		uart.port.set_divisor = loongson_frac_set_divisor;
	}

	if (flags & LOONGSON_UART_QUIRK_MCR)
		ddata->mcr_invert |= (UART_MCR_RTS | UART_MCR_DTR);

	if (flags & LOONGSON_UART_QUIRK_MSR)
		ddata->msr_invert |= (UART_MSR_CTS | UART_MSR_DSR);

	ddata->rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (IS_ERR(ddata->rst))
		return PTR_ERR(ddata->rst);

	ret = reset_control_deassert(ddata->rst);
	if (ret)
		return ret;

	ret = serial8250_register_8250_port(&uart);
	if (ret < 0) {
		reset_control_assert(ddata->rst);
		return ret;
	}

	ddata->line = ret;
	platform_set_drvdata(pdev, ddata);

	return 0;
}

static void loongson_uart_remove(struct platform_device *pdev)
{
	struct loongson_uart_data *ddata = platform_get_drvdata(pdev);

	serial8250_unregister_port(ddata->line);
	reset_control_assert(ddata->rst);
}

static int loongson_uart_suspend(struct device *dev)
{
	struct loongson_uart_data *ddata = dev_get_drvdata(dev);

	serial8250_suspend_port(ddata->line);

	return 0;
}

static int loongson_uart_resume(struct device *dev)
{
	struct loongson_uart_data *data = dev_get_drvdata(dev);

	serial8250_resume_port(data->line);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(loongson_uart_pm_ops, loongson_uart_suspend,
				loongson_uart_resume);

static const struct of_device_id loongson_uart_of_ids[] = {
	{ .compatible = "loongson,ls2k0500-uart", .data = (void *)LS2K0500_UART_FLAG },
	{ .compatible = "loongson,ls2k1500-uart", .data = (void *)LS2K1500_UART_FLAG },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, loongson_uart_of_ids);

static struct platform_driver loongson_uart_driver = {
	.probe = loongson_uart_probe,
	.remove = loongson_uart_remove,
	.driver = {
		.name = "loongson-uart",
		.pm = pm_ptr(&loongson_uart_pm_ops),
		.of_match_table = loongson_uart_of_ids,
	},
};

module_platform_driver(loongson_uart_driver);

MODULE_DESCRIPTION("Loongson UART driver");
MODULE_AUTHOR("Loongson Technology Corporation Limited.");
MODULE_LICENSE("GPL");
