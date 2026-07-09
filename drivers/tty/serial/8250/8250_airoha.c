// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha UART driver
 *
 * Copyright (c) 2025 Genexis Sweden AB
 * Author: Benjamin Larsson <benjamin.larsson@genexis.eu>
 *	   Christian Marangi <ansuelsmth@gmail.com>
 */

#include <linux/property.h>
#include <linux/serial_reg.h>
#include <linux/serial_8250.h>

#include "8250.h"

#define UART_AIROHA_BRDL	0
#define UART_AIROHA_BRDH	1
#define UART_AIROHA_XINCLKDR	10
#define UART_AIROHA_XYD		11

struct airoha_8250_priv {
	int line;
};

struct airoha_8250_data {
	unsigned int type;
};

struct airoha_8250_clk_div_info {
	int div;
	int mask;
};

#define UART_BRDL_20M		0x01
#define UART_BRDH_20M		0x00

#define XINDIV_CLOCK		20000000
#define XYD_Y			65000

static const struct airoha_8250_clk_div_info airoha_clk_div_info[] = {
	{ .div = 10, .mask = BIT(2) },
	{ .div = 4, .mask = BIT(1) },
	{ .div = 2, .mask = BIT(0) },
};

static const int clock_div_tab[] = { 10, 4, 2};
static const int clock_div_reg[] = {  4, 2, 1};

/*
 * Airoha UART baud rate calculation logic
 *
 * crystal_clock = 20 MHz (fixed frequency)
 * xindiv_clock = crystal_clock / clock_div
 * (x/y) = XYD, 32 bit register with 16 bits of x and then 16 bits of y
 * clock_div = XINCLK_DIVCNT (default set to 10 (0x4)),
 *           - 3 bit register [ 1, 2, 4, 8, 10, 12, 16, 20 ]
 *
 * baud_rate = ((xindiv_clock) * (x/y)) / ([BRDH,BRDL] * 16)
 *
 * Selecting divider needs to fulfill
 * 1.8432 MHz <= xindiv_clk <= APB clock / 2
 * The clocks are unknown but a divider of value 1 did not result in a valid
 * waveform.
 *
 * XYD_y seems to need to be larger then XYD_x for proper waveform generation.
 * Setting [BRDH,BRDL] to [0,1] and XYD_y to 65000 gives even values
 * for usual baud rates.
 */
static void airoha_set_termios(struct uart_port *port, struct ktermios *termios,
			       const struct ktermios *old)
{
	const struct airoha_8250_clk_div_info *clk_div_info;
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned int xyd_x, nom, denom;
	unsigned int baud;
	int i;

	serial8250_do_set_termios(port, termios, old);

	baud = serial8250_get_baud_rate(port, termios, old);

	/* Set DLAB to access the baud rate divider registers (BRDH, BRDL) */
	serial_port_out(port, UART_LCR, up->lcr | UART_LCR_DLAB);

	/* Set baud rate calculation defaults (BRDIV ([BRDH,BRDL]) to 1) */
	serial_port_out(port, UART_AIROHA_BRDL, UART_BRDL_20M);
	serial_port_out(port, UART_AIROHA_BRDH, UART_BRDH_20M);

	/*
	 * Calculate XYD_x and XINCLKDR register by searching
	 * through a table of crystal_clock divisors.
	 */
	for (i = 0 ; i < ARRAY_SIZE(airoha_clk_div_info) ; i++) {
		clk_div_info = &airoha_clk_div_info[i];

		denom = (XINDIV_CLOCK / 40) / clk_div_info->div;
		nom = baud * (XYD_Y / 40);
		xyd_x = ((nom / denom) << 4);
		/* For the HSUART xyd_x needs to be scaled by a factor of 2 */
		if (port->type == UART_PORT_AIROHA_HS)
			xyd_x = xyd_x >> 1;
		if (xyd_x < XYD_Y)
			break;
	}

	serial_port_out(port, UART_AIROHA_XINCLKDR, clk_div_info->mask);
	serial_port_out(port, UART_AIROHA_XYD, (xyd_x << 16) | XYD_Y);

	/* unset DLAB */
	serial_port_out(port, UART_LCR, up->lcr);
}

static int airoha_8250_probe(struct platform_device *pdev)
{
	const struct airoha_8250_data *data;
	struct uart_8250_port uart = { };
	struct device *dev = &pdev->dev;
	struct airoha_8250_priv *priv;
	struct resource *res;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -EINVAL, "invalid address\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	data = device_get_match_data(dev);

	uart.port.dev = dev;
	uart.port.type = data->type;
	uart.port.flags = UPF_BOOT_AUTOCONF | UPF_FIXED_PORT |
			  UPF_FIXED_TYPE | UPF_IOREMAP;
	uart.port.set_termios = airoha_set_termios;
	uart.port.mapbase = res->start;
	uart.port.mapsize = resource_size(res);

	ret = uart_read_and_validate_port_properties(&uart.port);
	if (ret)
		return ret;

	ret = serial8250_register_8250_port(&uart);
	if (ret < 0)
		return ret;

	priv->line = ret;
	platform_set_drvdata(pdev, priv);

	return 0;
}

static void airoha_8250_remove(struct platform_device *ofdev)
{
	struct airoha_8250_priv *priv = platform_get_drvdata(ofdev);

	serial8250_unregister_port(priv->line);
}

static const struct airoha_8250_data en7523_data = {
	.type = UART_PORT_AIROHA,
};

static const struct airoha_8250_data an7581_hs_data = {
	.type = UART_PORT_AIROHA_HS,
};

static const struct of_device_id airoha_8250_dt_ids[] = {
	{ .compatible = "airoha,en7523-uart", .data = &en7523_data, },
	{ .compatible = "airoha,an7581-hsuart", .data = &an7581_hs_data, },
	{ },
};
MODULE_DEVICE_TABLE(of, airoha_8250_dt_ids);

static struct platform_driver airoha_8250_driver = {
	.driver = {
		.name = "8250_airoha",
		.of_match_table = airoha_8250_dt_ids,
	},
	.probe = airoha_8250_probe,
	.remove = airoha_8250_remove,
};

module_platform_driver(airoha_8250_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha UART driver");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
