// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Serial Port driver for PRUSS UART on TI platforms
 *
 *  Copyright (C) 2025 by Texas Instruments Incorporated - http://www.ti.com/
 *  Author: Bin Liu <b-liu@ti.com>
 */
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/serial_reg.h>
#include <linux/serial_core.h>

#include "8250.h"

/* PowerManagement and Emulation */
#define PRUSS_UART_PEREMU_MGMT	12
#define PRUSS_UART_TX_EN	BIT(14)
#define PRUSS_UART_RX_EN	BIT(13)
#define PRUSS_UART_FREE_RUN	BIT(0)

/* Oversampling Mode Select */
#define PRUSS_UART_MDR			13
#define PRUSS_UART_MDR_OSM_SEL_MASK	BIT(0)
#define PRUSS_UART_MDR_16X_MODE		0
#define PRUSS_UART_MDR_13X_MODE		1

struct pruss8250_data {
	struct clk *clk;
	int line;
};

static int pruss8250_startup(struct uart_port *port)
{
	int ret;

	port->serial_out(port, PRUSS_UART_PEREMU_MGMT, 0);

	ret = serial8250_do_startup(port);
	if (!ret)
		port->serial_out(port, PRUSS_UART_PEREMU_MGMT, PRUSS_UART_TX_EN |
							       PRUSS_UART_RX_EN |
							       PRUSS_UART_FREE_RUN);

	return ret;
}

static unsigned int pruss8250_get_divisor(struct uart_port *port,
					  unsigned int baud,
					  unsigned int *frac)
{
	unsigned int uartclk = port->uartclk;
	unsigned int div_13, div_16;
	unsigned int abs_d13, abs_d16;
	u16 quot;

	div_13 = DIV_ROUND_CLOSEST(uartclk, 13 * baud);
	div_16 = DIV_ROUND_CLOSEST(uartclk, 16 * baud);
	div_13 = div_13 ? : 1;
	div_16 = div_16 ? : 1;

	abs_d13 = abs(baud - uartclk / 13 / div_13);
	abs_d16 = abs(baud - uartclk / 16 / div_16);

	if (abs_d13 >= abs_d16) {
		*frac = PRUSS_UART_MDR_16X_MODE;
		quot = div_16;
	} else {
		*frac = PRUSS_UART_MDR_13X_MODE;
		quot = div_13;
	}

	return quot;
}

static void pruss8250_set_divisor(struct uart_port *port, unsigned int baud,
				  unsigned int quot, unsigned int quot_frac)
{
	serial8250_do_set_divisor(port, baud, quot);

	/*
	 * quot_frac holds the MDR over-sampling mode
	 * which is set in pruss8250_get_divisor()
	 */
	quot_frac &= PRUSS_UART_MDR_OSM_SEL_MASK;
	port->serial_out(port, PRUSS_UART_MDR, quot_frac);
}

static int pruss8250_probe(struct platform_device *pdev)
{
	struct uart_8250_port port8250;
	struct uart_port *port = &port8250.port;
	struct device *dev = &pdev->dev;
	struct pruss8250_data *data;
	struct resource *res;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	memset(&port8250, 0, sizeof(port8250));

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get resource");
		return -EINVAL;
	}

	if (!port->uartclk) {
		data->clk = devm_clk_get(dev, NULL);
		if (IS_ERR(data->clk)) {
			dev_err(dev, "Failed to get clock!\n");
			return -ENODEV;
		} else {
			port->uartclk = clk_get_rate(data->clk);
			devm_clk_put(dev, data->clk);
		}
	}

	port->dev = dev;
	port->mapbase = res->start;
	port->mapsize = resource_size(res);
	port->type = PORT_16550A;
	port->flags = UPF_BOOT_AUTOCONF | UPF_FIXED_PORT | UPF_FIXED_TYPE |
		      UPF_IOREMAP;
	port->startup = pruss8250_startup;
	port->rs485_config = serial8250_em485_config;
	port->get_divisor = pruss8250_get_divisor;
	port->set_divisor = pruss8250_set_divisor;

	ret = uart_read_port_properties(port);
	if (ret)
		return ret;

	port->iotype = UPIO_MEM32;
	port->regshift = 2;

	spin_lock_init(&port8250.port.lock);
	port8250.capabilities = UART_CAP_FIFO | UART_CAP_AFE;

	ret = serial8250_register_8250_port(&port8250);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Unable to register 8250 port.\n");

	data->line = ret;
	platform_set_drvdata(pdev, data);
	return 0;
}

static void pruss8250_remove(struct platform_device *pdev)
{
	struct pruss8250_data *data = platform_get_drvdata(pdev);

	serial8250_unregister_port(data->line);
}

static const struct of_device_id pruss8250_of_match[] = {
	{ .compatible = "ti,pruss-uart", .data = (void *)PORT_16550A },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, pruss8250_of_match);

static struct platform_driver pruss8250_driver = {
	.driver = {
		.name = "pruss8250",
		.of_match_table = pruss8250_of_match,
	},
	.probe = pruss8250_probe,
	.remove = pruss8250_remove,
};

module_platform_driver(pruss8250_driver);

MODULE_AUTHOR("Bin Liu <b-liu@ti.com");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Serial Port driver for PRUSS UART on TI platforms");
