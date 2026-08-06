// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/clk.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/serial_reg.h>
#include <linux/serial_core.h>
#include <linux/sysrq.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define UART_RB_OFFSET			0x00
#define UART_TH_OFFSET			0x00
#define UART_DLL_OFFSET			0x00
#define UART_IE_OFFSET			0x04
#define UART_DLH_OFFSET			0x04
#define UART_II_OFFSET			0x08
#define UART_FC_OFFSET			0x08
#define UART_LC_OFFSET			0x0c
#define UART_MC_OFFSET			0x10
#define UART_LS_OFFSET			0x14
#define UART_MS_OFFSET			0x18
#define UART_US_OFFSET			0x7c
#define UART_SRR_OFFSET			0x88

#define UART_IE_ERETOI			0x40
#define UART_IE_ETOI			0x20
#define UART_IE_EDSSI			0x08
#define UART_IE_ELSI			0x04
#define UART_IE_ETBEI			0x02
#define UART_IE_ERBFI			0x01

#define UART_II_MODEM_STATUS_CHANGED	0x00
#define UART_II_NO_INT_PENDING		0x01
#define UART_II_THR_EMPTY		0x02
#define UART_II_RCV_DATA_AVAIL		0x04
#define UART_II_RCV_STATUS		0x06
#define UART_II_CHAR_TIMEOUT		0x0c
#define UART_II_CHAR_TIMEOUT_FIFO_EMPTY	0x0d

#define UART_FC_RX_2_TO_FULL		0xc0
#define UART_FC_TX_EMPTY		0x00
#define UART_FC_XMITR			0x04
#define UART_FC_RCVRR			0x02
#define UART_FC_FIFOE			0x01

#define UART_LC_DLAB			0x80
#define UART_LC_BRK			0x40
#define UART_LC_EVEN_PARITY		0x10
#define UART_LC_ODD_PARITY		0x00
#define UART_LC_PEN			0x08
#define UART_LC_STOP_2BIT		0x04
#define UART_LC_STOP_1BIT		0x00
#define UART_LC_CLS_8_BITS		0x03
#define UART_LC_CLS_7_BITS		0x02
#define UART_LC_CLS_6_BITS		0x01
#define UART_LC_CLS_5_BITS		0x00

#define UART_MC_AFCE			0x20
#define UART_MC_LB			0x10
#define UART_MC_OUT2			0x08
#define UART_MC_OUT1			0x04
#define UART_MC_RTS			0x02
#define UART_MC_DTR			0x01

#define UART_LS_TEMT			0x40
#define UART_LS_THRE			0x20
#define UART_LS_BI			0x10
#define UART_LS_FE			0x08
#define UART_LS_PE			0x04
#define UART_LS_OE			0x02
#define UART_LS_DR			0x01

#define UART_MS_DCD			0x80
#define UART_MS_RI			0x40
#define UART_MS_DSR			0x20
#define UART_MS_CTS			0x10
#define UART_MS_DDCD			0x08
#define UART_MS_DCTS			0x01

#define UART_US_TFNF			0x02

#define UART_FIFO_SIZE			64

#define DEFAULT_AMBARELLA_UART_MCR	0
#define DEFAULT_AMBARELLA_UART_IER	(UART_IE_ELSI | UART_IE_ERBFI | \
					 UART_IE_ETOI)

#define AMBA_UART_MAX_NUM		8

#define AMBA_UART_RESET_FLAG		0 /* bit 0 */

/* Poll timeout in microseconds (atomic helpers use udelay). */
#define AMBARELLA_UART_TIMEOUT_US	1000000

struct ambarella_uart_port {
	struct uart_port port;
	struct clk *uart_pll;
	unsigned long flags;
	u32 mcr;
	/* Software copy of UART_IE, updated with the port lock held. */
	u32 ier;
	bool console_line_ended;
};

static struct ambarella_uart_port ambarella_port[AMBA_UART_MAX_NUM];

static struct ambarella_uart_port *
to_ambarella_uart_port(struct uart_port *port)
{
	return container_of(port, struct ambarella_uart_port, port);
}

static void serial_ambarella_ier_write(struct uart_port *port, u32 ier)
{
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);

	amb_port->ier = ier;
	writel_relaxed(ier, port->membase + UART_IE_OFFSET);
}

static void serial_ambarella_ier_set(struct uart_port *port, u32 set)
{
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);

	amb_port->ier |= set;
	writel_relaxed(amb_port->ier, port->membase + UART_IE_OFFSET);
}

static void serial_ambarella_ier_clear(struct uart_port *port, u32 clear)
{
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);

	amb_port->ier &= ~clear;
	writel_relaxed(amb_port->ier, port->membase + UART_IE_OFFSET);
}

static void serial_ambarella_ier_toggle(struct uart_port *port, u32 mask)
{
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);

	amb_port->ier &= ~mask;
	writel_relaxed(amb_port->ier, port->membase + UART_IE_OFFSET);
	amb_port->ier |= mask;
	writel_relaxed(amb_port->ier, port->membase + UART_IE_OFFSET);
}

static void __serial_ambarella_stop_tx(struct uart_port *port)
{
	serial_ambarella_ier_clear(port, UART_IE_ETBEI);
}

static u32 __serial_ambarella_read_ms(struct uart_port *port)
{
	return readl_relaxed(port->membase + UART_MS_OFFSET);
}

static void __serial_ambarella_enable_ms(struct uart_port *port)
{
	serial_ambarella_ier_set(port, UART_IE_EDSSI);
}

static void __serial_ambarella_disable_ms(struct uart_port *port)
{
	serial_ambarella_ier_clear(port, UART_IE_EDSSI);
}

static inline void wait_for_tx(struct uart_port *port)
{
	u32 ls;
	int ret;

	ret = readl_poll_timeout_atomic(port->membase + UART_LS_OFFSET, ls,
					ls & UART_LS_TEMT, 1,
					AMBARELLA_UART_TIMEOUT_US);
	if (likely(!ret))
		return;

	/* Recover a stuck TX path so console/poll can continue. */
	writel_relaxed(UART_FC_RX_2_TO_FULL | UART_FC_TX_EMPTY |
			UART_FC_XMITR | UART_FC_RCVRR,
			port->membase + UART_FC_OFFSET);
	udelay(100);
	writel_relaxed(UART_FC_FIFOE | UART_FC_RX_2_TO_FULL |
			UART_FC_TX_EMPTY | UART_FC_XMITR |
			UART_FC_RCVRR,
			port->membase + UART_FC_OFFSET);
}

static inline int wait_for_rx(struct uart_port *port)
{
	u32 ls;

	return readl_relaxed_poll_timeout_atomic(port->membase + UART_LS_OFFSET,
						 ls, ls & UART_LS_DR, 1,
						 AMBARELLA_UART_TIMEOUT_US);
}

static inline int tx_fifo_is_full(struct uart_port *port)
{
	return !(readl_relaxed(port->membase + UART_US_OFFSET) & UART_US_TFNF);
}

static void serial_ambarella_hw_setup(struct uart_port *port)
{
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);

	if (!test_and_set_bit(AMBA_UART_RESET_FLAG, &amb_port->flags)) {
		if (amb_port->uart_pll)
			port->uartclk = clk_get_rate(amb_port->uart_pll);
		/* reset the whole UART only once */
		writel_relaxed(0x01, port->membase + UART_SRR_OFFSET);
		mdelay(1);
		writel_relaxed(0x00, port->membase + UART_SRR_OFFSET);
	}

	writel_relaxed(UART_FC_FIFOE | UART_FC_RX_2_TO_FULL | UART_FC_TX_EMPTY |
			UART_FC_XMITR | UART_FC_RCVRR, port->membase + UART_FC_OFFSET);
	/* Keep interrupts disabled until the IRQ handler is registered. */
	serial_ambarella_ier_write(port, 0);
}

static inline void serial_ambarella_receive_chars(struct uart_port *port,
						  u32 tmo)
{
	u32 ch, flag, ls;
	bool have_char;
	int max_count;

	ls = readl_relaxed(port->membase + UART_LS_OFFSET);
	max_count = port->fifosize;

	do {
		flag = TTY_NORMAL;
		have_char = ls & UART_LS_DR;
		if (have_char || (ls & UART_LS_BI) || tmo)
			ch = readl_relaxed(port->membase + UART_RB_OFFSET);
		if (have_char) {
			port->icount.rx++;
			tmo = 0;
		}

		if (unlikely(ls & (UART_LS_BI | UART_LS_PE |
					UART_LS_FE | UART_LS_OE))) {
			if (ls & UART_LS_BI) {
				ls &= ~(UART_LS_FE | UART_LS_PE);
				port->icount.brk++;

				if (uart_handle_break(port))
					goto ignore_char;
			}
			if (ls & UART_LS_FE)
				port->icount.frame++;
			if (ls & UART_LS_PE)
				port->icount.parity++;
			if (ls & UART_LS_OE)
				port->icount.overrun++;

			ls &= port->read_status_mask;

			if (ls & UART_LS_BI)
				flag = TTY_BREAK;
			else if (ls & UART_LS_FE)
				flag = TTY_FRAME;
			else if (ls & UART_LS_PE)
				flag = TTY_PARITY;
			else if (ls & UART_LS_OE)
				flag = TTY_OVERRUN;

			if (ls & UART_LS_OE)
				pr_debug("%s: OVERFLOW\n", __func__);
		}

		if (have_char) {
			if (uart_handle_sysrq_char(port, ch))
				goto ignore_char;

			uart_insert_char(port, ls, UART_LS_OE, ch, flag);
		}

ignore_char:
		ls = readl_relaxed(port->membase + UART_LS_OFFSET);
	} while ((ls & (UART_LS_DR | UART_LS_BI)) && (max_count-- > 0));

	tty_flip_buffer_push(&port->state->port);
}

static void serial_ambarella_transmit_chars(struct uart_port *port)
{
	struct tty_port *tport = &port->state->port;
	int count;

	if (port->x_char) {
		writel_relaxed(port->x_char, port->membase + UART_TH_OFFSET);
		port->icount.tx++;
		port->x_char = 0;
		return;
	}

	if (uart_tx_stopped(port) || kfifo_is_empty(&tport->xmit_fifo)) {
		__serial_ambarella_stop_tx(port);
		return;
	}

	count = port->fifosize;
	while (count-- > 0) {
		unsigned char c;

		if (tx_fifo_is_full(port))
			break;

		if (!kfifo_peek(&tport->xmit_fifo, &c))
			break;

		writel_relaxed(c, port->membase + UART_TH_OFFSET);
		kfifo_skip(&tport->xmit_fifo);
		port->icount.tx++;
		if (kfifo_is_empty(&tport->xmit_fifo))
			break;
	}

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);
	if (kfifo_is_empty(&tport->xmit_fifo))
		__serial_ambarella_stop_tx(port);
}

static inline void serial_ambarella_check_modem_status(struct uart_port *port)
{
	u32 ms;

	ms = __serial_ambarella_read_ms(port);

	if (ms & UART_MS_RI)
		port->icount.rng++;
	if (ms & UART_MS_DSR)
		port->icount.dsr++;
	if (ms & UART_MS_DCTS)
		uart_handle_cts_change(port, (ms & UART_MS_CTS));
	if (ms & UART_MS_DDCD)
		uart_handle_dcd_change(port, (ms & UART_MS_DCD));

	wake_up_interruptible(&port->state->port.delta_msr_wait);
}

static irqreturn_t serial_ambarella_irq(int irq, void *dev_id)
{
	struct uart_port *port = dev_id;
	u32 ii;

	scoped_guard(uart_port_lock_irqsave, port) {
		ii = readl_relaxed(port->membase + UART_II_OFFSET);
		switch (ii & 0x0F) {
		case UART_II_MODEM_STATUS_CHANGED:
			serial_ambarella_check_modem_status(port);
			break;
		case UART_II_THR_EMPTY:
			serial_ambarella_transmit_chars(port);
			break;
		case UART_II_RCV_STATUS:
		case UART_II_RCV_DATA_AVAIL:
			serial_ambarella_receive_chars(port, 0);
			break;
		case UART_II_CHAR_TIMEOUT_FIFO_EMPTY:
			/* Clear ERETOI to dismiss timeout-with-empty-FIFO IRQ */
			serial_ambarella_ier_toggle(port, UART_IE_ERETOI);
			fallthrough;
		case UART_II_CHAR_TIMEOUT:
			serial_ambarella_receive_chars(port, 1);
			break;
		case UART_II_NO_INT_PENDING:
			break;
		default:
			pr_debug("%s: 0x%x\n", __func__, ii);
			break;
		}
	}

	return IRQ_HANDLED;
}

static void serial_ambarella_enable_ms(struct uart_port *port)
{
	__serial_ambarella_enable_ms(port);
}

static void serial_ambarella_start_tx(struct uart_port *port)
{
	/* if transmit buffer is not allocated, just return */
	if (!port->state->port.xmit_buf)
		return;

	serial_ambarella_ier_set(port, UART_IE_ETBEI);
	serial_ambarella_transmit_chars(port);
}

static void serial_ambarella_stop_tx(struct uart_port *port)
{
	__serial_ambarella_stop_tx(port);
}

static void serial_ambarella_stop_rx(struct uart_port *port)
{
	serial_ambarella_ier_clear(port, UART_IE_ERBFI);
}

static unsigned int serial_ambarella_tx_empty(struct uart_port *port)
{
	unsigned int lsr;

	guard(uart_port_lock_irqsave)(port);
	lsr = readl_relaxed(port->membase + UART_LS_OFFSET);

	return ((lsr & (UART_LS_TEMT | UART_LS_THRE)) ==
		(UART_LS_TEMT | UART_LS_THRE)) ? TIOCSER_TEMT : 0;
}

static unsigned int serial_ambarella_get_mctrl(struct uart_port *port)
{
	u32 ms, mctrl = 0;

	ms = __serial_ambarella_read_ms(port);

	if (ms & UART_MS_CTS)
		mctrl |= TIOCM_CTS;
	if (ms & UART_MS_DSR)
		mctrl |= TIOCM_DSR;
	if (ms & UART_MS_RI)
		mctrl |= TIOCM_RI;
	if (ms & UART_MS_DCD)
		mctrl |= TIOCM_CD;

	return mctrl;
}

static void serial_ambarella_set_mctrl(struct uart_port *port,
				       unsigned int mctrl)
{
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);
	u32 mcr, mcr_new = 0;

	mcr = readl_relaxed(port->membase + UART_MC_OFFSET);

	if (mctrl & TIOCM_DTR)
		mcr_new |= UART_MC_DTR;
	if (mctrl & TIOCM_RTS)
		mcr_new |= UART_MC_RTS;
	if (mctrl & TIOCM_OUT1)
		mcr_new |= UART_MC_OUT1;
	if (mctrl & TIOCM_OUT2)
		mcr_new |= UART_MC_OUT2;
	if (mctrl & TIOCM_LOOP)
		mcr_new |= UART_MC_LB;

	mcr_new |= amb_port->mcr;
	if (mcr_new != mcr) {
		if ((mcr & UART_MC_AFCE) == UART_MC_AFCE) {
			mcr &= ~UART_MC_AFCE;
			writel_relaxed(mcr, port->membase + UART_MC_OFFSET);
		}
		writel_relaxed(mcr_new, port->membase + UART_MC_OFFSET);
	}
}

static void serial_ambarella_break_ctl(struct uart_port *port, int break_state)
{
	u32 lcr;

	guard(uart_port_lock_irqsave)(port);
	lcr = readl_relaxed(port->membase + UART_LC_OFFSET);
	if (break_state != 0)
		writel_relaxed(lcr | UART_LC_BRK, port->membase + UART_LC_OFFSET);
	else
		writel_relaxed(lcr & ~UART_LC_BRK, port->membase + UART_LC_OFFSET);
}

static void serial_ambarella_hw_deinit(struct ambarella_uart_port *amb_port)
{
	struct uart_port *port = &amb_port->port;

	/* Disable interrupts */
	serial_ambarella_ier_write(port, 0);

	/* Reset the Rx and Tx FIFOs */
	writel_relaxed(UART_FCR_CLEAR_XMIT | UART_FCR_CLEAR_RCVR,
		       port->membase + UART_SRR_OFFSET);
}

static int serial_ambarella_startup(struct uart_port *port)
{
	int rval;
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);

	serial_ambarella_hw_setup(port);

	rval = request_irq(port->irq, serial_ambarella_irq, IRQF_TRIGGER_HIGH,
			   dev_name(amb_port->port.dev), &amb_port->port);
	if (rval < 0) {
		dev_err(amb_port->port.dev,
			"Failed to register ISR for IRQ %d\n", port->irq);
		serial_ambarella_hw_deinit(amb_port);
		return rval;
	}

	serial_ambarella_ier_write(port, DEFAULT_AMBARELLA_UART_IER);

	return 0;
}

static void serial_ambarella_shutdown(struct uart_port *port)
{
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);
	u32 lcr;

	scoped_guard(uart_port_lock_irqsave, port) {
		serial_ambarella_hw_deinit(amb_port);
		lcr = readl_relaxed(port->membase + UART_LC_OFFSET);
		writel_relaxed(lcr & ~UART_LC_BRK, port->membase + UART_LC_OFFSET);
	}

	free_irq(amb_port->port.irq, &amb_port->port);
}

static void serial_ambarella_set_termios(struct uart_port *port,
					 struct ktermios *termios,
					 const struct ktermios *old)
{
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);
	unsigned int baud, quot;
	u32 lc = 0x0;

	port->uartclk = clk_get_rate(amb_port->uart_pll);
	switch (termios->c_cflag & CSIZE) {
	case CS5:
		lc |= UART_LC_CLS_5_BITS;
		break;
	case CS6:
		lc |= UART_LC_CLS_6_BITS;
		break;
	case CS7:
		lc |= UART_LC_CLS_7_BITS;
		break;
	case CS8:
	default:
		lc |= UART_LC_CLS_8_BITS;
		break;
	}

	if (termios->c_cflag & CSTOPB)
		lc |= UART_LC_STOP_2BIT;
	else
		lc |= UART_LC_STOP_1BIT;

	if (termios->c_cflag & PARENB) {
		if (termios->c_cflag & PARODD)
			lc |= (UART_LC_PEN | UART_LC_ODD_PARITY);
		else
			lc |= (UART_LC_PEN | UART_LC_EVEN_PARITY);
	}

	baud = uart_get_baud_rate(port, termios, old, 0, port->uartclk / 16);
	quot = uart_get_divisor(port, baud);

	scoped_guard(uart_port_lock_irqsave, port) {
		uart_update_timeout(port, termios->c_cflag, baud);

		port->read_status_mask = UART_LSR_OE | UART_LSR_THRE | UART_LSR_DR;
		if (termios->c_iflag & INPCK)
			port->read_status_mask |= UART_LSR_FE | UART_LSR_PE;
		if (termios->c_iflag & (BRKINT | PARMRK))
			port->read_status_mask |= UART_LSR_BI;

		port->ignore_status_mask = 0;
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= UART_LSR_PE | UART_LSR_FE;
		if (termios->c_iflag & IGNBRK) {
			port->ignore_status_mask |= UART_LSR_BI;
			if (termios->c_iflag & IGNPAR)
				port->ignore_status_mask |= UART_LSR_OE;
		}
		if ((termios->c_cflag & CREAD) == 0)
			port->ignore_status_mask |= UART_LSR_DR;

		if ((termios->c_cflag & CRTSCTS) == 0) {
			amb_port->mcr &= ~UART_MC_AFCE;
			port->status &= ~UPSTAT_AUTOCTS;
		} else {
			amb_port->mcr |= UART_MC_AFCE;
			port->status |= UPSTAT_AUTOCTS;
		}

		writel_relaxed(UART_LC_DLAB, port->membase + UART_LC_OFFSET);
		writel_relaxed(quot & 0xff, port->membase + UART_DLL_OFFSET);
		writel_relaxed((quot >> 8) & 0xff, port->membase + UART_DLH_OFFSET);
		writel_relaxed(lc, port->membase + UART_LC_OFFSET);
		if (UART_ENABLE_MS(port, termios->c_cflag))
			__serial_ambarella_enable_ms(port);
		else
			__serial_ambarella_disable_ms(port);
		serial_ambarella_set_mctrl(port, port->mctrl);
	}
}

static void serial_ambarella_pm(struct uart_port *port,
				unsigned int state, unsigned int oldstate)
{
}

static void serial_ambarella_release_port(struct uart_port *port)
{
}

static int serial_ambarella_request_port(struct uart_port *port)
{
	return 0;
}

static void serial_ambarella_config_port(struct uart_port *port, int flags)
{
}

static int serial_ambarella_verify_port(struct uart_port *port,
					struct serial_struct *ser)
{
	int rval = 0;

	if (ser->type != PORT_UNKNOWN && ser->type != PORT_UART00)
		rval = -EINVAL;
	if (port->irq != ser->irq)
		rval = -EINVAL;
	if (ser->io_type != SERIAL_IO_MEM)
		rval = -EINVAL;

	return rval;
}

static const char *serial_ambarella_type(struct uart_port *port)
{
	return "ambuart";
}

#ifdef CONFIG_CONSOLE_POLL
static void serial_ambarella_poll_put_char(struct uart_port *port,
					   unsigned char chr)
{
	if (!port->suspended) {
		wait_for_tx(port);
		writel_relaxed(chr, port->membase + UART_TH_OFFSET);
	}
}

static int serial_ambarella_poll_get_char(struct uart_port *port)
{
	if (port->suspended)
		return NO_POLL_CHAR;

	if (wait_for_rx(port))
		return NO_POLL_CHAR;

	return readl_relaxed(port->membase + UART_RB_OFFSET);
}
#endif

static const struct uart_ops serial_ambarella_pops = {
	.tx_empty	= serial_ambarella_tx_empty,
	.set_mctrl	= serial_ambarella_set_mctrl,
	.get_mctrl	= serial_ambarella_get_mctrl,
	.stop_tx	= serial_ambarella_stop_tx,
	.start_tx	= serial_ambarella_start_tx,
	.stop_rx	= serial_ambarella_stop_rx,
	.enable_ms	= serial_ambarella_enable_ms,
	.break_ctl	= serial_ambarella_break_ctl,
	.startup	= serial_ambarella_startup,
	.shutdown	= serial_ambarella_shutdown,
	.set_termios	= serial_ambarella_set_termios,
	.pm		= serial_ambarella_pm,
	.type		= serial_ambarella_type,
	.release_port	= serial_ambarella_release_port,
	.request_port	= serial_ambarella_request_port,
	.config_port	= serial_ambarella_config_port,
	.verify_port	= serial_ambarella_verify_port,
#ifdef CONFIG_CONSOLE_POLL
	.poll_put_char	= serial_ambarella_poll_put_char,
	.poll_get_char	= serial_ambarella_poll_get_char,
#endif
};

#if defined(CONFIG_SERIAL_AMBARELLA_CONSOLE)

static struct uart_driver serial_ambarella_reg;

static void serial_ambarella_putchar(struct uart_port *port, unsigned char ch)
{
	wait_for_tx(port);
	writel_relaxed(ch, port->membase + UART_TH_OFFSET);
}

static void serial_ambarella_console_putchar(struct uart_port *port,
					     unsigned char ch)
{
	struct ambarella_uart_port *amb_port = to_ambarella_uart_port(port);

	serial_ambarella_putchar(port, ch);
	amb_port->console_line_ended = (ch == '\n');
}

static void serial_ambarella_console_device_lock(struct console *co,
						 unsigned long *flags)
{
	__uart_port_lock_irqsave(&ambarella_port[co->index].port, flags);
}

static void serial_ambarella_console_device_unlock(struct console *co,
						   unsigned long flags)
{
	__uart_port_unlock_irqrestore(&ambarella_port[co->index].port, flags);
}

static void serial_ambarella_console_write_atomic(struct console *co,
						  struct nbcon_write_context *wctxt)
{
	struct ambarella_uart_port *amb_port = &ambarella_port[co->index];
	struct uart_port *port = &amb_port->port;

	if (port->suspended)
		return;

	if (!nbcon_enter_unsafe(wctxt))
		return;

	if (!amb_port->console_line_ended)
		uart_console_write(port, "\n", 1, serial_ambarella_console_putchar);
	uart_console_write(port, wctxt->outbuf, wctxt->len,
			   serial_ambarella_console_putchar);
	wait_for_tx(port);

	nbcon_exit_unsafe(wctxt);
}

static void serial_ambarella_console_write_thread(struct console *co,
						  struct nbcon_write_context *wctxt)
{
	struct ambarella_uart_port *amb_port = &ambarella_port[co->index];
	struct uart_port *port = &amb_port->port;

	if (port->suspended)
		return;

	if (!nbcon_enter_unsafe(wctxt))
		return;

	if (nbcon_exit_unsafe(wctxt)) {
		unsigned int len = READ_ONCE(wctxt->len);
		unsigned int i;

		/*
		 * Toggle unsafe per byte so a higher-priority context can
		 * take over. After a failed enter/exit, outbuf/len are no
		 * longer trusted and printing must stop.
		 */
		for (i = 0; i < len; i++) {
			if (!nbcon_enter_unsafe(wctxt))
				break;
			uart_console_write(port, wctxt->outbuf + i, 1,
					   serial_ambarella_console_putchar);
			if (!nbcon_exit_unsafe(wctxt))
				break;
		}
	}

	while (!nbcon_enter_unsafe(wctxt))
		nbcon_reacquire_nobuf(wctxt);

	wait_for_tx(port);
	nbcon_exit_unsafe(wctxt);
}

static int __init serial_ambarella_console_setup(struct console *co,
						 char *options)
{
	struct uart_port *port;
	struct ambarella_uart_port *amb_port;
	int baud = 115200, bits = 8, parity = 'n', flow = 'n';

	if (co->index < 0 || co->index >= serial_ambarella_reg.nr)
		co->index = 0;

	amb_port = &ambarella_port[co->index];
	port = &amb_port->port;
	if (!port->membase) {
		pr_err("No device available for serial console\n");
		return -ENODEV;
	}

	port->ops = &serial_ambarella_pops;
	port->line = co->index;
	amb_port->console_line_ended = true;

	serial_ambarella_hw_setup(port);

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static struct console serial_ambarella_console = {
	.name		= "ttyAB",
	.device		= uart_console_device,
	.setup		= serial_ambarella_console_setup,
	.write_atomic	= serial_ambarella_console_write_atomic,
	.write_thread	= serial_ambarella_console_write_thread,
	.device_lock	= serial_ambarella_console_device_lock,
	.device_unlock	= serial_ambarella_console_device_unlock,
	.flags		= CON_PRINTBUFFER | CON_ANYTIME | CON_NBCON,
	.index		= -1,
	.data		= &serial_ambarella_reg,
};

static void serial_ambarella_console_early_write(struct console *con,
						 const char *s,
						 unsigned int count)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, count, serial_ambarella_putchar);
}

static int __init serial_ambarella_console_early_setup(struct earlycon_device *dev,
						       const char *opt)
{
	if (!dev->port.membase)
		return -ENODEV;

	dev->con->write = serial_ambarella_console_early_write;

	return 0;
}

OF_EARLYCON_DECLARE(ambarella_uart, "ambarella,cv75-uart",
		    serial_ambarella_console_early_setup);

#define AMBARELLA_CONSOLE	(&serial_ambarella_console)
#else
#define AMBARELLA_CONSOLE	NULL
#endif

static struct uart_driver serial_ambarella_reg = {
	.owner		= THIS_MODULE,
	.driver_name	= "ambarella-uart",
	.dev_name	= "ttyAB",
	.major		= 0,
	.minor		= 0,
	.nr		= AMBA_UART_MAX_NUM,
	.cons		= AMBARELLA_CONSOLE,
};

static int serial_ambarella_probe(struct platform_device *pdev)
{
	struct ambarella_uart_port *amb_port;
	struct resource *mem;
	struct pinctrl *pinctrl;
	int irq, id, rval;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "no mem resource!\n");
		return -ENODEV;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no irq resource!\n");
		return -ENODEV;
	}

	id = of_alias_get_id(pdev->dev.of_node, "serial");
	if (id < 0 || id >= serial_ambarella_reg.nr) {
		dev_err(&pdev->dev, "Invalid uart ID %d!\n", id);
		return -ENXIO;
	}

	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl)) {
		dev_err(&pdev->dev, "Failed to request pinctrl\n");
		return PTR_ERR(pinctrl);
	}

	amb_port = &ambarella_port[id];

	amb_port->uart_pll = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(amb_port->uart_pll)) {
		dev_err(&pdev->dev, "Get uart clk failed!\n");
		return PTR_ERR(amb_port->uart_pll);
	}

	amb_port->mcr = DEFAULT_AMBARELLA_UART_MCR;

	amb_port->port.dev = &pdev->dev;
	amb_port->port.type = PORT_UART00;
	amb_port->port.iotype = UPIO_MEM;
	amb_port->port.fifosize = UART_FIFO_SIZE;
	amb_port->port.uartclk = clk_get_rate(amb_port->uart_pll);
	amb_port->port.ops = &serial_ambarella_pops;
	amb_port->port.irq = irq;
	amb_port->port.line = id;
	amb_port->port.mapbase = mem->start;
	amb_port->port.membase = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(amb_port->port.membase))
		return PTR_ERR(amb_port->port.membase);

	rval = uart_add_one_port(&serial_ambarella_reg, &amb_port->port);
	if (rval < 0)
		dev_err(&pdev->dev, "failed to add port: %d, %d!\n", id, rval);

	platform_set_drvdata(pdev, amb_port);

	return rval;
}

static void serial_ambarella_remove(struct platform_device *pdev)
{
	struct ambarella_uart_port *amb_port;

	amb_port = platform_get_drvdata(pdev);
	uart_remove_one_port(&serial_ambarella_reg, &amb_port->port);
}

static int serial_ambarella_suspend(struct device *dev)
{
	struct ambarella_uart_port *amb_port = dev_get_drvdata(dev);

	return uart_suspend_port(&serial_ambarella_reg, &amb_port->port);
}

static int serial_ambarella_resume(struct device *dev)
{
	struct ambarella_uart_port *amb_port = dev_get_drvdata(dev);

	clear_bit(AMBA_UART_RESET_FLAG, &amb_port->flags);
	serial_ambarella_hw_setup(&amb_port->port);

	return uart_resume_port(&serial_ambarella_reg, &amb_port->port);
}

static DEFINE_SIMPLE_DEV_PM_OPS(serial_ambarella_pm_ops,
				serial_ambarella_suspend,
				serial_ambarella_resume);

static const struct of_device_id ambarella_serial_of_match[] = {
	{ .compatible = "ambarella,cv75-uart" },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_serial_of_match);

static struct platform_driver serial_ambarella_driver = {
	.probe		= serial_ambarella_probe,
	.remove		= serial_ambarella_remove,
	.driver		= {
		.name	= "ambarella-uart",
		.of_match_table = ambarella_serial_of_match,
		.pm = pm_sleep_ptr(&serial_ambarella_pm_ops),
	},
};

static int __init serial_ambarella_init(void)
{
	int rval;

	rval = uart_register_driver(&serial_ambarella_reg);
	if (rval < 0)
		return rval;

	rval = platform_driver_register(&serial_ambarella_driver);
	if (rval < 0) {
		uart_unregister_driver(&serial_ambarella_reg);
		return rval;
	}

	return 0;
}

static void __exit serial_ambarella_exit(void)
{
	platform_driver_unregister(&serial_ambarella_driver);
	uart_unregister_driver(&serial_ambarella_reg);
}

module_init(serial_ambarella_init);
module_exit(serial_ambarella_exit);

MODULE_DESCRIPTION("Ambarella UART driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ambarella-uart");
