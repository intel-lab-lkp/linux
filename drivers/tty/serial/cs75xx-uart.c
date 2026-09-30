// SPDX-License-Identifier: GPL-2.0
/*
 * UART driver for the Cortina Systems CS75xx (Goldengate G2) SoCs
 *
 * Based on the Cortina Systems vendor driver.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define CS75XX_UART_CFG			0x00
#define   CS75XX_UART_CFG_BAUD		GENMASK(31, 8)
#define   CS75XX_UART_CFG_EN		BIT(7)
#define   CS75XX_UART_CFG_RX_EN		BIT(6)
#define   CS75XX_UART_CFG_TX_EN		BIT(5)
#define   CS75XX_UART_CFG_PARITY_EN	BIT(4)
#define   CS75XX_UART_CFG_PARITY_EVEN	BIT(3)
#define   CS75XX_UART_CFG_STOP_2BIT	BIT(2)
#define   CS75XX_UART_CFG_CHAR		GENMASK(1, 0)
#define CS75XX_UART_RX_SAMPLE		0x08
#define CS75XX_UART_TXDAT		0x10
#define CS75XX_UART_RXDAT		0x14
#define   CS75XX_UART_RXDAT_VALID	BIT(8)
#define   CS75XX_UART_RXDAT_DATA	GENMASK(7, 0)
#define CS75XX_UART_INFO		0x18
#define   CS75XX_UART_INFO_TX_EMPTY	BIT(3)
#define   CS75XX_UART_INFO_TX_FULL	BIT(2)
#define   CS75XX_UART_INFO_RX_EMPTY	BIT(1)
#define CS75XX_UART_IE			0x1c
#define CS75XX_UART_INT			0x24

/* Interrupt bits, shared by the IE and INT registers */
#define CS75XX_UART_IRQ_RX_NONEMPTY	BIT(6)
#define CS75XX_UART_IRQ_TX_EMPTY	BIT(5)
#define CS75XX_UART_IRQ_RX_OVERRUN	BIT(3)
#define CS75XX_UART_IRQ_RX_PARITY	BIT(2)
#define CS75XX_UART_IRQ_RX_STOP		BIT(1)
#define CS75XX_UART_IRQ_ALL		GENMASK(6, 0)

#define CS75XX_UART_DIV_MIN		16
#define CS75XX_UART_DIV_MAX		FIELD_MAX(CS75XX_UART_CFG_BAUD)

#define CS75XX_UART_NR_PORTS		4
#define CS75XX_UART_FIFO_SIZE		16
#define CS75XX_UART_POLL_TIMEOUT_US	10000

struct cs75xx_uart_port {
	struct uart_port port;
	struct clk *clk;
};

static struct cs75xx_uart_port *cs75xx_uart_ports[CS75XX_UART_NR_PORTS];

static void cs75xx_uart_ie_update(struct uart_port *port, u32 clear, u32 set)
{
	u32 ie = readl(port->membase + CS75XX_UART_IE);

	writel((ie & ~clear) | set, port->membase + CS75XX_UART_IE);
}

static void __maybe_unused cs75xx_uart_putchar(struct uart_port *port,
					       unsigned char ch)
{
	u32 info;

	readl_poll_timeout_atomic(port->membase + CS75XX_UART_INFO, info,
				  !(info & CS75XX_UART_INFO_TX_FULL), 1,
				  CS75XX_UART_POLL_TIMEOUT_US);
	writel(ch, port->membase + CS75XX_UART_TXDAT);
}

static unsigned int cs75xx_uart_tx_empty(struct uart_port *port)
{
	u32 info = readl(port->membase + CS75XX_UART_INFO);

	return (info & CS75XX_UART_INFO_TX_EMPTY) ? TIOCSER_TEMT : 0;
}

static void cs75xx_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int cs75xx_uart_get_mctrl(struct uart_port *port)
{
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void cs75xx_uart_stop_tx(struct uart_port *port)
{
	cs75xx_uart_ie_update(port, CS75XX_UART_IRQ_TX_EMPTY, 0);
}

static void cs75xx_uart_tx_chars(struct uart_port *port)
{
	u8 ch;

	uart_port_tx(port, ch,
		     !(readl(port->membase + CS75XX_UART_INFO) &
		       CS75XX_UART_INFO_TX_FULL),
		     writel(ch, port->membase + CS75XX_UART_TXDAT));
}

static void cs75xx_uart_start_tx(struct uart_port *port)
{
	cs75xx_uart_ie_update(port, 0, CS75XX_UART_IRQ_TX_EMPTY);

	/*
	 * The TX empty interrupt only fires when the FIFO drains, so prime
	 * the FIFO here.
	 */
	cs75xx_uart_tx_chars(port);
}

static void cs75xx_uart_stop_rx(struct uart_port *port)
{
	cs75xx_uart_ie_update(port, CS75XX_UART_IRQ_RX_NONEMPTY, 0);
}

static void cs75xx_uart_break_ctl(struct uart_port *port, int break_state)
{
}

static void cs75xx_uart_rx_chars(struct uart_port *port, u32 status)
{
	struct tty_port *tport = &port->state->port;
	u32 data;

	if (status & CS75XX_UART_IRQ_RX_OVERRUN) {
		port->icount.overrun++;
		tty_insert_flip_char(tport, 0, TTY_OVERRUN);
	}
	if (status & CS75XX_UART_IRQ_RX_PARITY)
		port->icount.parity++;
	if (status & CS75XX_UART_IRQ_RX_STOP)
		port->icount.frame++;

	while (!(readl(port->membase + CS75XX_UART_INFO) &
		 CS75XX_UART_INFO_RX_EMPTY)) {
		u8 ch;

		data = readl(port->membase + CS75XX_UART_RXDAT);
		if (!(data & CS75XX_UART_RXDAT_VALID))
			continue;

		port->icount.rx++;
		ch = FIELD_GET(CS75XX_UART_RXDAT_DATA, data);

		if (port->ignore_status_mask & CS75XX_UART_RXDAT_VALID)
			continue;
		if (uart_prepare_sysrq_char(port, ch))
			continue;

		tty_insert_flip_char(tport, ch, TTY_NORMAL);
	}

	tty_flip_buffer_push(tport);
}

static irqreturn_t cs75xx_uart_isr(int irq, void *dev_id)
{
	struct uart_port *port = dev_id;
	u32 status;

	uart_port_lock(port);

	status = readl(port->membase + CS75XX_UART_INT);
	writel(status, port->membase + CS75XX_UART_INT);

	cs75xx_uart_rx_chars(port, status);
	cs75xx_uart_tx_chars(port);

	uart_unlock_and_check_sysrq(port);

	return IRQ_HANDLED;
}

static int cs75xx_uart_startup(struct uart_port *port)
{
	unsigned long flags;
	u32 cfg;
	int ret;

	writel(0, port->membase + CS75XX_UART_IE);
	writel(CS75XX_UART_IRQ_ALL, port->membase + CS75XX_UART_INT);

	ret = request_irq(port->irq, cs75xx_uart_isr, 0, dev_name(port->dev),
			  port);
	if (ret)
		return ret;

	uart_port_lock_irqsave(port, &flags);

	cfg = readl(port->membase + CS75XX_UART_CFG);
	cfg |= CS75XX_UART_CFG_EN | CS75XX_UART_CFG_RX_EN | CS75XX_UART_CFG_TX_EN;
	writel(cfg, port->membase + CS75XX_UART_CFG);

	writel(CS75XX_UART_IRQ_RX_NONEMPTY, port->membase + CS75XX_UART_IE);

	uart_port_unlock_irqrestore(port, flags);

	return 0;
}

static void cs75xx_uart_shutdown(struct uart_port *port)
{
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	writel(0, port->membase + CS75XX_UART_IE);
	uart_port_unlock_irqrestore(port, flags);

	free_irq(port->irq, port);
}

static void cs75xx_uart_set_termios(struct uart_port *port,
				    struct ktermios *termios,
				    const struct ktermios *old)
{
	unsigned int baud, div;
	unsigned long flags;
	u32 cfg;

	/* No hardware flow control or mark/space parity support */
	termios->c_cflag &= ~(CRTSCTS | CMSPAR);

	baud = uart_get_baud_rate(port, termios, old,
				  DIV_ROUND_UP(port->uartclk, CS75XX_UART_DIV_MAX),
				  port->uartclk / CS75XX_UART_DIV_MIN);
	div = DIV_ROUND_CLOSEST(port->uartclk, baud);

	cfg = FIELD_PREP(CS75XX_UART_CFG_BAUD, div) | CS75XX_UART_CFG_EN |
	      CS75XX_UART_CFG_RX_EN | CS75XX_UART_CFG_TX_EN;
	cfg |= FIELD_PREP(CS75XX_UART_CFG_CHAR,
			  tty_get_char_size(termios->c_cflag) - 5);
	if (termios->c_cflag & CSTOPB)
		cfg |= CS75XX_UART_CFG_STOP_2BIT;
	if (termios->c_cflag & PARENB) {
		cfg |= CS75XX_UART_CFG_PARITY_EN;
		if (!(termios->c_cflag & PARODD))
			cfg |= CS75XX_UART_CFG_PARITY_EVEN;
	}

	uart_port_lock_irqsave(port, &flags);

	port->ignore_status_mask = 0;
	if (!(termios->c_cflag & CREAD))
		port->ignore_status_mask |= CS75XX_UART_RXDAT_VALID;

	uart_update_timeout(port, termios->c_cflag, baud);

	writel(cfg, port->membase + CS75XX_UART_CFG);
	/* Sample the received bits in the middle of the bit time */
	writel(div / 2, port->membase + CS75XX_UART_RX_SAMPLE);

	uart_port_unlock_irqrestore(port, flags);

	if (tty_termios_baud_rate(termios))
		tty_termios_encode_baud_rate(termios, baud, baud);
}

static const char *cs75xx_uart_type(struct uart_port *port)
{
	return port->type == PORT_GENERIC ? "CS75XX" : NULL;
}

static void cs75xx_uart_release_port(struct uart_port *port)
{
}

static int cs75xx_uart_request_port(struct uart_port *port)
{
	return 0;
}

static void cs75xx_uart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_GENERIC;
}

static int cs75xx_uart_verify_port(struct uart_port *port,
				   struct serial_struct *serinfo)
{
	if (serinfo->type != PORT_UNKNOWN && serinfo->type != PORT_GENERIC)
		return -EINVAL;
	if (serinfo->irq != port->irq)
		return -EINVAL;

	return 0;
}

#ifdef CONFIG_CONSOLE_POLL
static int cs75xx_uart_poll_get_char(struct uart_port *port)
{
	if (readl(port->membase + CS75XX_UART_INFO) & CS75XX_UART_INFO_RX_EMPTY)
		return NO_POLL_CHAR;

	return FIELD_GET(CS75XX_UART_RXDAT_DATA,
			 readl(port->membase + CS75XX_UART_RXDAT));
}

static void cs75xx_uart_poll_put_char(struct uart_port *port, unsigned char c)
{
	cs75xx_uart_putchar(port, c);
}
#endif

static const struct uart_ops cs75xx_uart_ops = {
	.tx_empty	= cs75xx_uart_tx_empty,
	.set_mctrl	= cs75xx_uart_set_mctrl,
	.get_mctrl	= cs75xx_uart_get_mctrl,
	.stop_tx	= cs75xx_uart_stop_tx,
	.start_tx	= cs75xx_uart_start_tx,
	.stop_rx	= cs75xx_uart_stop_rx,
	.break_ctl	= cs75xx_uart_break_ctl,
	.startup	= cs75xx_uart_startup,
	.shutdown	= cs75xx_uart_shutdown,
	.set_termios	= cs75xx_uart_set_termios,
	.type		= cs75xx_uart_type,
	.release_port	= cs75xx_uart_release_port,
	.request_port	= cs75xx_uart_request_port,
	.config_port	= cs75xx_uart_config_port,
	.verify_port	= cs75xx_uart_verify_port,
#ifdef CONFIG_CONSOLE_POLL
	.poll_get_char	= cs75xx_uart_poll_get_char,
	.poll_put_char	= cs75xx_uart_poll_put_char,
#endif
};

#ifdef CONFIG_SERIAL_CS75XX_CONSOLE
static void cs75xx_uart_console_write(struct console *co, const char *s,
				      unsigned int count)
{
	struct uart_port *port = &cs75xx_uart_ports[co->index]->port;
	unsigned long flags;
	bool locked = true;
	u32 ie, info;

	if (oops_in_progress)
		locked = uart_port_trylock_irqsave(port, &flags);
	else
		uart_port_lock_irqsave(port, &flags);

	ie = readl(port->membase + CS75XX_UART_IE);
	writel(ie & ~CS75XX_UART_IRQ_TX_EMPTY, port->membase + CS75XX_UART_IE);

	uart_console_write(port, s, count, cs75xx_uart_putchar);

	readl_poll_timeout_atomic(port->membase + CS75XX_UART_INFO, info,
				  info & CS75XX_UART_INFO_TX_EMPTY, 1,
				  CS75XX_UART_POLL_TIMEOUT_US);
	writel(ie, port->membase + CS75XX_UART_IE);

	if (locked)
		uart_port_unlock_irqrestore(port, flags);
}

static int __init cs75xx_uart_console_setup(struct console *co, char *options)
{
	struct cs75xx_uart_port *up;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	if (co->index < 0 || co->index >= CS75XX_UART_NR_PORTS)
		return -ENODEV;

	up = cs75xx_uart_ports[co->index];
	if (!up)
		return -ENODEV;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(&up->port, co, baud, parity, bits, flow);
}

static struct uart_driver cs75xx_uart_driver;

static struct console cs75xx_uart_console = {
	.name	= "ttyCS",
	.write	= cs75xx_uart_console_write,
	.device	= uart_console_device,
	.setup	= cs75xx_uart_console_setup,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
	.data	= &cs75xx_uart_driver,
};

static void cs75xx_uart_earlycon_write(struct console *co, const char *s,
				       unsigned int count)
{
	struct earlycon_device *dev = co->data;

	uart_console_write(&dev->port, s, count, cs75xx_uart_putchar);
}

static int __init cs75xx_uart_earlycon_setup(struct earlycon_device *dev,
					     const char *options)
{
	if (!dev->port.membase)
		return -ENODEV;

	dev->con->write = cs75xx_uart_earlycon_write;

	return 0;
}

OF_EARLYCON_DECLARE(cs75xx, "cortina,cs7542-uart", cs75xx_uart_earlycon_setup);

#define CS75XX_UART_CONSOLE	(&cs75xx_uart_console)
#else
#define CS75XX_UART_CONSOLE	NULL
#endif

static struct uart_driver cs75xx_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "cs75xx-uart",
	.dev_name	= "ttyCS",
	.nr		= CS75XX_UART_NR_PORTS,
	.cons		= CS75XX_UART_CONSOLE,
};

static int cs75xx_uart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cs75xx_uart_port *up;
	struct uart_port *port;
	struct resource *res;
	int id, ret;

	id = of_alias_get_id(dev->of_node, "serial");
	if (id < 0) {
		for (id = 0; id < CS75XX_UART_NR_PORTS; id++)
			if (!cs75xx_uart_ports[id])
				break;
	}
	if (id >= CS75XX_UART_NR_PORTS)
		return dev_err_probe(dev, -EINVAL, "invalid port index %d\n", id);
	if (cs75xx_uart_ports[id])
		return dev_err_probe(dev, -EBUSY, "port %d already in use\n", id);

	up = devm_kzalloc(dev, sizeof(*up), GFP_KERNEL);
	if (!up)
		return -ENOMEM;

	port = &up->port;

	port->membase = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(port->membase))
		return PTR_ERR(port->membase);

	port->irq = platform_get_irq(pdev, 0);
	if (port->irq < 0)
		return port->irq;

	up->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(up->clk))
		return dev_err_probe(dev, PTR_ERR(up->clk), "failed to get clock\n");

	port->uartclk = clk_get_rate(up->clk);
	if (!port->uartclk)
		return dev_err_probe(dev, -EINVAL, "clock rate is zero\n");

	port->dev = dev;
	port->mapbase = res->start;
	port->mapsize = resource_size(res);
	port->iotype = UPIO_MEM32;
	port->ops = &cs75xx_uart_ops;
	port->line = id;
	port->fifosize = CS75XX_UART_FIFO_SIZE;
	port->flags = UPF_BOOT_AUTOCONF;
	port->has_sysrq = IS_ENABLED(CONFIG_SERIAL_CS75XX_CONSOLE);

	cs75xx_uart_ports[id] = up;
	platform_set_drvdata(pdev, up);

	ret = uart_add_one_port(&cs75xx_uart_driver, port);
	if (ret) {
		cs75xx_uart_ports[id] = NULL;
		return ret;
	}

	return 0;
}

static void cs75xx_uart_remove(struct platform_device *pdev)
{
	struct cs75xx_uart_port *up = platform_get_drvdata(pdev);

	uart_remove_one_port(&cs75xx_uart_driver, &up->port);
	cs75xx_uart_ports[up->port.line] = NULL;
}

static const struct of_device_id cs75xx_uart_of_match[] = {
	{ .compatible = "cortina,cs7542-uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, cs75xx_uart_of_match);

static struct platform_driver cs75xx_uart_platform_driver = {
	.probe	= cs75xx_uart_probe,
	.remove	= cs75xx_uart_remove,
	.driver	= {
		.name		= "cs75xx-uart",
		.of_match_table	= cs75xx_uart_of_match,
	},
};

static int __init cs75xx_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&cs75xx_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&cs75xx_uart_platform_driver);
	if (ret)
		uart_unregister_driver(&cs75xx_uart_driver);

	return ret;
}
module_init(cs75xx_uart_init);

static void __exit cs75xx_uart_exit(void)
{
	platform_driver_unregister(&cs75xx_uart_platform_driver);
	uart_unregister_driver(&cs75xx_uart_driver);
}
module_exit(cs75xx_uart_exit);

MODULE_DESCRIPTION("Cortina Systems CS75xx UART driver");
MODULE_LICENSE("GPL");
