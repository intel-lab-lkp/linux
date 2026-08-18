// SPDX-License-Identifier: GPL-2.0
/*
 * Serial port driver for Ambarella UART
 *
 * Copyright (C) 2026 Ambarella, Inc.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_8250.h>
#include <linux/serial_reg.h>

#include "8250.h"

#define AMBARELLA_UART_IER_ETOI		BIT(5)
#define AMBARELLA_UART_USR		0x1f
#define AMBARELLA_UART_USR_BUSY		BIT(0)
#define AMBARELLA_UART_IIR_IID		GENMASK(3, 0)
#define AMBARELLA_UART_IIR_STATUS	GENMASK(5, 0)

struct ambarella_uart_soc_data {
	unsigned int	fifo_size;
};

struct ambarella_uart {
	int					line;
	struct clk				*clk;
	const struct ambarella_uart_soc_data	*soc_data;
	bool					in_idle;
};

static void ambarella_idle_exit(struct uart_port *p)
{
	struct ambarella_uart *data = p->private_data;
	struct uart_8250_port *up = up_to_u8250p(p);

	if (up->capabilities & UART_CAP_FIFO)
		writel(up->fcr, p->membase + (UART_FCR << p->regshift));
	writel(up->mcr, p->membase + (UART_MCR << p->regshift));
	writel(up->ier, p->membase + (UART_IER << p->regshift));

	data->in_idle = false;
}

static int ambarella_idle_enter(struct uart_port *p)
{
	struct ambarella_uart *data = p->private_data;
	struct uart_8250_port *up = up_to_u8250p(p);
	unsigned int status;
	int retries = 4;

	lockdep_assert_held_once(&p->lock);

	data->in_idle = true;
	writel(0, p->membase + (UART_IER << p->regshift));

	serial8250_fifo_wait_for_lsr_thre(up, p->fifosize);
	ndelay(p->frame_time);
	writel(up->mcr | UART_MCR_LOOP,
	       p->membase + (UART_MCR << p->regshift));

	do {
		serial8250_clear_fifos(up);
		if (!(readl(p->membase +
			    (AMBARELLA_UART_USR << p->regshift)) &
		      AMBARELLA_UART_USR_BUSY))
			break;
		ndelay(p->frame_time);
	} while (--retries);

	status = serial_lsr_in(up);
	if (status & UART_LSR_DR) {
		readl(p->membase + (UART_RX << p->regshift));
		up->lsr_saved_flags = 0;
	}

	if (readl(p->membase + (AMBARELLA_UART_USR << p->regshift)) &
	    AMBARELLA_UART_USR_BUSY) {
		ambarella_idle_exit(p);
		return -EBUSY;
	}

	return 0;
}

static void ambarella_serial_out(struct uart_port *p, unsigned int offset,
				 u32 value)
{
	struct ambarella_uart *data = p->private_data;
	u32 lcr;

	if (offset != UART_LCR || !data || data->in_idle) {
		writel(value, p->membase + (offset << p->regshift));
		return;
	}

	lcr = readl(p->membase + (UART_LCR << p->regshift));
	if (lcr == value)
		return;

	writel(value, p->membase + (UART_LCR << p->regshift));
	lcr = readl(p->membase + (UART_LCR << p->regshift));
	if ((lcr & ~UART_LCR_SPAR) == (value & ~UART_LCR_SPAR))
		return;

	if (ambarella_idle_enter(p))
		return;

	writel(value, p->membase + (UART_LCR << p->regshift));
	ambarella_idle_exit(p);
}

static u32 ambarella_serial_in(struct uart_port *p, unsigned int offset)
{
	return readl(p->membase + (offset << p->regshift));
}

static int ambarella_handle_irq(struct uart_port *p)
{
	struct uart_8250_port *up = up_to_u8250p(p);
	unsigned int iir = serial_port_in(p, UART_IIR);
	bool rx_timeout = (iir & AMBARELLA_UART_IIR_STATUS) ==
			  UART_IIR_RX_TIMEOUT;
	unsigned int status;

	guard(uart_port_lock_check_sysrq_irqsave)(p);

	switch (FIELD_GET(AMBARELLA_UART_IIR_IID, iir)) {
	case UART_IIR_NO_INT:
		return 0;
	case UART_IIR_BUSY:
		serial_port_in(p, AMBARELLA_UART_USR);
		return 1;
	}

	if (rx_timeout) {
		status = serial_lsr_in(up);
		if (!(status & (UART_LSR_DR | UART_LSR_BI)))
			serial_port_in(p, UART_RX);
	}

	serial8250_handle_irq_locked(p, iir);
	return 1;
}

static void ambarella_set_divisor(struct uart_port *p, unsigned int baud,
				  unsigned int quot, unsigned int quot_frac)
{
	struct uart_8250_port *up = up_to_u8250p(p);

	if (ambarella_idle_enter(p))
		return;

	serial_port_out(p, UART_LCR, up->lcr | UART_LCR_DLAB);
	if (serial_port_in(p, UART_LCR) & UART_LCR_DLAB)
		serial_dl_write(up, quot);
	serial_port_out(p, UART_LCR, up->lcr);

	ambarella_idle_exit(p);
}

static int ambarella_startup(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	int ret;

	ret = serial8250_do_startup(port);
	if (ret)
		return ret;

	up->ier |= AMBARELLA_UART_IER_ETOI;
	serial_port_out(port, UART_IER, up->ier);

	return 0;
}

static int ambarella_uart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ambarella_uart *data;
	struct uart_8250_port uart = {};
	struct resource *regs;
	u32 reg_io_width = 4;
	u32 reg_shift = 2;
	int irq, ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!regs)
		return dev_err_probe(dev, -EINVAL, "missing registers\n");

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->soc_data = device_get_match_data(dev);
	if (!data->soc_data)
		return -EINVAL;

	data->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(data->clk))
		return dev_err_probe(dev, PTR_ERR(data->clk),
				     "unable to get clock\n");

	uart.port.dev = dev;
	uart.port.mapbase = regs->start;
	uart.port.mapsize = resource_size(regs);
	uart.port.membase = devm_ioremap(dev, regs->start,
					 resource_size(regs));
	if (!uart.port.membase)
		return -ENOMEM;

	uart.port.irq = irq;
	uart.port.type = PORT_16550A;
	uart.port.flags = UPF_FIXED_PORT | UPF_FIXED_TYPE | UPF_SKIP_TEST;
	uart.port.startup = ambarella_startup;
	uart.port.handle_irq = ambarella_handle_irq;
	uart.port.set_divisor = ambarella_set_divisor;
	uart.port.uartclk = clk_get_rate(data->clk);
	if (!uart.port.uartclk)
		return dev_err_probe(dev, -EINVAL, "clock rate not defined\n");

	uart.port.fifosize = data->soc_data->fifo_size;
	uart.port.private_data = data;
	uart.capabilities = UART_CAP_FIFO | UART_CAP_NOTEMT | UART_CAP_AFE;

	of_property_read_u32(dev->of_node, "reg-shift", &reg_shift);
	of_property_read_u32(dev->of_node, "reg-io-width", &reg_io_width);
	uart.port.regshift = reg_shift;

	switch (reg_io_width) {
	case 4:
		uart.port.iotype = UPIO_MEM32;
		uart.port.serial_in = ambarella_serial_in;
		uart.port.serial_out = ambarella_serial_out;
		break;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "unsupported reg-io-width %u\n",
				     reg_io_width);
	}

	ret = serial8250_register_8250_port(&uart);
	if (ret < 0)
		return dev_err_probe(dev, ret, "unable to register 8250 port\n");

	data->line = ret;
	platform_set_drvdata(pdev, data);
	return 0;
}

static void ambarella_uart_remove(struct platform_device *pdev)
{
	struct ambarella_uart *data = platform_get_drvdata(pdev);

	serial8250_unregister_port(data->line);
}

static const struct ambarella_uart_soc_data cv75_uart_data = {
	.fifo_size = 64,
};

static const struct of_device_id ambarella_uart_dt_ids[] = {
	{
		.compatible = "ambarella,cv75-uart",
		.data = &cv75_uart_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ambarella_uart_dt_ids);

static struct platform_driver ambarella_uart_platform_driver = {
	.driver = {
		.name		= "8250-ambarella",
		.of_match_table	= ambarella_uart_dt_ids,
	},
	.probe	= ambarella_uart_probe,
	.remove	= ambarella_uart_remove,
};

module_platform_driver(ambarella_uart_platform_driver);

static int __init ambarella_early_setup(struct earlycon_device *device,
					const char *options)
{
	struct uart_port *port = &device->port;

	if (!port->membase)
		return -ENODEV;

	port->iotype = UPIO_MEM32;
	port->regshift = 2;
	port->serial_in = ambarella_serial_in;
	port->serial_out = ambarella_serial_out;

	return early_serial8250_setup(device, options);
}

OF_EARLYCON_DECLARE(ambarella, "ambarella,cv75-uart", ambarella_early_setup);

MODULE_IMPORT_NS("SERIAL_8250");
MODULE_AUTHOR("Long Zhao <longzhao@ambarella.com>");
MODULE_DESCRIPTION("Ambarella UART driver");
MODULE_LICENSE("GPL");
