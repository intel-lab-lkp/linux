/* SPDX-License-Identifier: GPL-2.0+ */
/*
 *  Driver for 8250/16550-type serial ports
 *
 *  Based on drivers/char/serial.c, by Linus Torvalds, Theodore Ts'o.
 *
 *  Copyright (C) 2001 Russell King.
 */

#include <linux/bits.h>
#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/dmaengine.h>

#include "../serial_mctrl_gpio.h"

struct uart_8250_dma {
	int (*tx_dma)(struct uart_8250_port *p);
	int (*rx_dma)(struct uart_8250_port *p);
	void (*prepare_tx_dma)(struct uart_8250_port *p);
	void (*prepare_rx_dma)(struct uart_8250_port *p);

	/* Filter function */
	dma_filter_fn		fn;
	/* Parameter to the filter function */
	void			*rx_param;
	void			*tx_param;

	struct dma_slave_config	rxconf;
	struct dma_slave_config	txconf;

	struct dma_chan		*rxchan;
	struct dma_chan		*txchan;

	/* Device address base for DMA operations */
	phys_addr_t		rx_dma_addr;
	phys_addr_t		tx_dma_addr;

	/* DMA address of the buffer in memory */
	dma_addr_t		rx_addr;
	dma_addr_t		tx_addr;

	dma_cookie_t		rx_cookie;
	dma_cookie_t		tx_cookie;

	void			*rx_buf;

	size_t			rx_size;
	size_t			tx_size;

	unsigned char		tx_running;
	unsigned char		tx_err;
	unsigned char		rx_running;
};

struct old_serial_port {
	unsigned int uart;
	unsigned int baud_base;
	unsigned int port;
	unsigned int irq;
	upf_t        flags;
	unsigned char io_type;
	unsigned char __iomem *iomem_base;
	unsigned short iomem_reg_shift;
};

struct serial8250_config {
	const char	*name;
	unsigned short	fifo_size;
	unsigned short	tx_loadsz;
	unsigned char	fcr;
	unsigned char	rxtrig_bytes[UART_FCR_R_TRIG_MAX_STATE];
	unsigned int	flags;
};

/*
 * The uart_config[] array index is referenced and defined from index
 * in uapi/linux/serial.h and uapi/linux/serial_core.h.
 *
 * This was old practice and for anything that doesn't need to support
 * userspace, new type should be limited and added only HERE.
 *
 * Any UART port that requires userspace support, should define the
 * dedicated index in the UAPI header and reference it when added
 * to this enum table.
 */
enum uart_port_type {
	/* From uapi/linux/serial.h */
	UART_PORT_UNKNOWN		= PORT_UNKNOWN, /* 0 */
	UART_PORT_8250			= PORT_8250,
	UART_PORT_16450			= PORT_16450,
	UART_PORT_16550			= PORT_16550,
	UART_PORT_16550A		= PORT_16550A,
	UART_PORT_CIRRUS		= PORT_CIRRUS,
	UART_PORT_16650			= PORT_16650,
	UART_PORT_16650V2		= PORT_16650V2,
	UART_PORT_16750			= PORT_16750,
	UART_PORT_STARTECH		= PORT_STARTECH,
	UART_PORT_16C950		= PORT_16C950,
	UART_PORT_16654			= PORT_16654,
	UART_PORT_16850			= PORT_16850,
	UART_PORT_RSA			= PORT_RSA, /* 13 */

	/* From uapi/linux/serial_core.h (14-123) */
	UART_PORT_NS16550A		= PORT_NS16550A, /* 14 */
	UART_PORT_XSCALE		= PORT_XSCALE,
	UART_PORT_RM9000		= PORT_RM9000,
	UART_PORT_OCTEON		= PORT_OCTEON,
	UART_PORT_AR7			= PORT_AR7,
	UART_PORT_U6_16550A		= PORT_U6_16550A,
	UART_PORT_TEGRA			= PORT_TEGRA,
	UART_PORT_XR17D15X		= PORT_XR17D15X,
	UART_PORT_LPC3220		= PORT_LPC3220,
	UART_PORT_8250_CIR		= PORT_8250_CIR,
	UART_PORT_XR17V35X		= PORT_XR17V35X,
	UART_PORT_BRCM_TRUMANAGE	= PORT_BRCM_TRUMANAGE,
	UART_PORT_ALTR_16550_F32	= PORT_ALTR_16550_F32,
	UART_PORT_ALTR_16550_F64	= PORT_ALTR_16550_F64,
	UART_PORT_ALTR_16550_F128	= PORT_ALTR_16550_F128,
	UART_PORT_RT2880		= PORT_RT2880,
	UART_PORT_16550A_FSL64		= PORT_16550A_FSL64,
	UART_PORT_PXA			= PORT_PXA,
	UART_PORT_AMBA			= PORT_AMBA,
	UART_PORT_CLPS711X		= PORT_CLPS711X,
	UART_PORT_SA1100		= PORT_SA1100,
	UART_PORT_UART00		= PORT_UART00,
	UART_PORT_OWL			= PORT_OWL,
	UART_PORT_21285			= PORT_21285,
	UART_PORT_SUNZILOG		= PORT_SUNZILOG,
	UART_PORT_SUNSAB		= PORT_SUNSAB,
	UART_PORT_NPCM			= PORT_NPCM,
	UART_PORT_TEGRA_TCU		= PORT_TEGRA_TCU,
	UART_PORT_ASPEED_VUART		= PORT_ASPEED_VUART,
	UART_PORT_PCH_8LINE		= PORT_PCH_8LINE,
	UART_PORT_PCH_2LINE		= PORT_PCH_2LINE,
	UART_PORT_DZ			= PORT_DZ,
	UART_PORT_ZS			= PORT_ZS,
	UART_PORT_MUX			= PORT_MUX,
	UART_PORT_ATMEL			= PORT_ATMEL,
	UART_PORT_MAC_ZILOG		= PORT_MAC_ZILOG,
	UART_PORT_PMAC_ZILOG		= PORT_PMAC_ZILOG,
	UART_PORT_SCI			= PORT_SCI,
	UART_PORT_SCIF			= PORT_SCIF,
	UART_PORT_IRDA			= PORT_IRDA,
	UART_PORT_IP22ZILOG		= PORT_IP22ZILOG,
	UART_PORT_CPM			= PORT_CPM,
	UART_PORT_MPC52xx		= PORT_MPC52xx,
	UART_PORT_ICOM			= PORT_ICOM,
	UART_PORT_IMX			= PORT_IMX,
	UART_PORT_TXX9			= PORT_TXX9,
	UART_PORT_JSM			= PORT_JSM,
	UART_PORT_SUNHV			= PORT_SUNHV,
	UART_PORT_UARTLITE		= PORT_UARTLITE,
	UART_PORT_BCM7271		= PORT_BCM7271,
	UART_PORT_SB1250_DUART		= PORT_SB1250_DUART,
	UART_PORT_MCF			= PORT_MCF,
	UART_PORT_SC26XX		= PORT_SC26XX,
	UART_PORT_SCIFA			= PORT_SCIFA,
	UART_PORT_S3C6400		= PORT_S3C6400,
	UART_PORT_MAX3100		= PORT_MAX3100,
	UART_PORT_TIMBUART		= PORT_TIMBUART,
	UART_PORT_MSM			= PORT_MSM,
	UART_PORT_BCM63XX		= PORT_BCM63XX,
	UART_PORT_APBUART		= PORT_APBUART,
	UART_PORT_ALTERA_JTAGUART	= PORT_ALTERA_JTAGUART,
	UART_PORT_ALTERA_UART		= PORT_ALTERA_UART,
	UART_PORT_SCIFB			= PORT_SCIFB,
	UART_PORT_MAX310X		= PORT_MAX310X,
	UART_PORT_DA830			= PORT_DA830,
	UART_PORT_OMAP			= PORT_OMAP,
	UART_PORT_VT8500		= PORT_VT8500,
	UART_PORT_XUARTPS		= PORT_XUARTPS,
	UART_PORT_AR933X		= PORT_AR933X,
	UART_PORT_MCHP16550A		= PORT_MCHP16550A,
	UART_PORT_ARC			= PORT_ARC,
	UART_PORT_RP2			= PORT_RP2,
	UART_PORT_LPUART		= PORT_LPUART,
	UART_PORT_HSCIF			= PORT_HSCIF,
	UART_PORT_ASC			= PORT_ASC,
	UART_PORT_MEN_Z135		= PORT_MEN_Z135,
	UART_PORT_SC16IS7XX		= PORT_SC16IS7XX,
	UART_PORT_MESON			= PORT_MESON,
	UART_PORT_DIGICOLOR		= PORT_DIGICOLOR,
	UART_PORT_SPRD			= PORT_SPRD,
	UART_PORT_STM32			= PORT_STM32,
	UART_PORT_MVEBU			= PORT_MVEBU,
	UART_PORT_PIC32			= PORT_PIC32,
	UART_PORT_MPS2UART		= PORT_MPS2UART,
	UART_PORT_MTK_BTIF		= PORT_MTK_BTIF,
	UART_PORT_RDA			= PORT_RDA,
	UART_PORT_MLB_USIO		= PORT_MLB_USIO,
	UART_PORT_SIFIVE_V0		= PORT_SIFIVE_V0,
	UART_PORT_SUNIX			= PORT_SUNIX,
	UART_PORT_LINFLEXUART		= PORT_LINFLEXUART,
	UART_PORT_SUNPLUS		= PORT_SUNPLUS, /* 123 */

	/* Internal 8250 only */
	UART_PORT_AIROHA		= 124,
	UART_PORT_AIROHA_HS		= 125,
};

#define UART_CAP_FIFO	BIT(8)	/* UART has FIFO */
#define UART_CAP_EFR	BIT(9)	/* UART has EFR */
#define UART_CAP_SLEEP	BIT(10)	/* UART has IER sleep */
#define UART_CAP_AFE	BIT(11)	/* MCR-based hw flow control */
#define UART_CAP_UUE	BIT(12)	/* UART needs IER bit 6 set (Xscale) */
#define UART_CAP_RTOIE	BIT(13)	/* UART needs IER bit 4 set (Xscale, Tegra) */
#define UART_CAP_HFIFO	BIT(14)	/* UART has a "hidden" FIFO */
#define UART_CAP_RPM	BIT(15)	/* Runtime PM is active while idle */
#define UART_CAP_IRDA	BIT(16)	/* UART supports IrDA line discipline */
#define UART_CAP_MINI	BIT(17)	/* Mini UART on BCM283X family lacks:
					 * STOP PARITY EPAR SPAR WLEN5 WLEN6
					 */
#define UART_CAP_NOTEMT	BIT(18)	/* UART without interrupt on TEMT available */

#define UART_BUG_QUOT	BIT(0)	/* UART has buggy quot LSB */
#define UART_BUG_TXEN	BIT(1)	/* UART has buggy TX IIR status */
#define UART_BUG_NOMSR	BIT(2)	/* UART has buggy MSR status bits (Au1x00) */
#define UART_BUG_THRE	BIT(3)	/* UART has buggy THRE reassertion */
#define UART_BUG_TXRACE	BIT(5)	/* UART Tx fails to set remote DR */

/* Module parameters */
#define UART_NR	CONFIG_SERIAL_8250_NR_UARTS

extern unsigned int nr_uarts;

#define SERIAL8250_PORT_FLAGS(_base, _irq, _flags)		\
	{							\
		.iobase		= _base,			\
		.irq		= _irq,				\
		.uartclk	= 1843200,			\
		.iotype		= UPIO_PORT,			\
		.flags		= UPF_BOOT_AUTOCONF | (_flags),	\
	}

#define SERIAL8250_PORT(_base, _irq) SERIAL8250_PORT_FLAGS(_base, _irq, 0)

extern struct uart_driver serial8250_reg;
void serial8250_register_ports(struct uart_driver *drv, struct device *dev);

/* Legacy ISA bus related APIs */
typedef void (*serial8250_isa_config_fn)(int, struct uart_port *, u32 *);
extern serial8250_isa_config_fn serial8250_isa_config;

void serial8250_isa_init_ports(void);

extern struct platform_device *serial8250_isa_devs;

extern const struct uart_ops *univ8250_port_base_ops;
extern struct uart_ops univ8250_port_ops;

static inline int serial_in(struct uart_8250_port *up, int offset)
{
	return up->port.serial_in(&up->port, offset);
}

static inline void serial_out(struct uart_8250_port *up, int offset, int value)
{
	up->port.serial_out(&up->port, offset, value);
}

/**
 *	serial_lsr_in - Read LSR register and preserve flags across reads
 *	@up:	uart 8250 port
 *
 *	Read LSR register and handle saving non-preserved flags across reads.
 *	The flags that are not preserved across reads are stored into
 *	up->lsr_saved_flags.
 *
 *	Returns LSR value or'ed with the preserved flags (if any).
 */
static inline u16 serial_lsr_in(struct uart_8250_port *up)
{
	u16 lsr = up->lsr_saved_flags;

	lsr |= serial_in(up, UART_LSR);
	up->lsr_saved_flags = lsr & up->lsr_save_mask;

	return lsr;
}

/*
 * For the 16C950
 */
static void serial_icr_write(struct uart_8250_port *up, int offset, int value)
{
	serial_out(up, UART_SCR, offset);
	serial_out(up, UART_ICR, value);
}

static unsigned int __maybe_unused serial_icr_read(struct uart_8250_port *up,
						   int offset)
{
	unsigned int value;

	serial_icr_write(up, UART_ACR, up->acr | UART_ACR_ICRRD);
	serial_out(up, UART_SCR, offset);
	value = serial_in(up, UART_ICR);
	serial_icr_write(up, UART_ACR, up->acr);

	return value;
}

void serial8250_clear_fifos(struct uart_8250_port *p);
void serial8250_clear_and_reinit_fifos(struct uart_8250_port *p);
void serial8250_fifo_wait_for_lsr_thre(struct uart_8250_port *up, unsigned int count);

void serial8250_rpm_get(struct uart_8250_port *p);
void serial8250_rpm_put(struct uart_8250_port *p);
DEFINE_GUARD(serial8250_rpm, struct uart_8250_port *,
	     serial8250_rpm_get(_T), serial8250_rpm_put(_T));

static inline u32 serial_dl_read(struct uart_8250_port *up)
{
	return up->dl_read(up);
}

static inline void serial_dl_write(struct uart_8250_port *up, u32 value)
{
	up->dl_write(up, value);
}

static inline bool serial8250_set_THRI(struct uart_8250_port *up)
{
	/* Port locked to synchronize UART_IER access against the console. */
	lockdep_assert_held_once(&up->port.lock);

	if (up->ier & UART_IER_THRI)
		return false;
	up->ier |= UART_IER_THRI;
	serial_out(up, UART_IER, up->ier);
	return true;
}

static inline bool serial8250_clear_THRI(struct uart_8250_port *up)
{
	/* Port locked to synchronize UART_IER access against the console. */
	lockdep_assert_held_once(&up->port.lock);

	if (!(up->ier & UART_IER_THRI))
		return false;
	up->ier &= ~UART_IER_THRI;
	serial_out(up, UART_IER, up->ier);
	return true;
}

struct uart_8250_port *serial8250_setup_port(int index);
struct uart_8250_port *serial8250_get_port(int line);

int serial8250_em485_config(struct uart_port *port, struct ktermios *termios,
			    struct serial_rs485 *rs485);
void serial8250_em485_start_tx(struct uart_8250_port *p, bool toggle_ier);
void serial8250_em485_stop_tx(struct uart_8250_port *p, bool toggle_ier);
void serial8250_em485_destroy(struct uart_8250_port *p);
extern struct serial_rs485 serial8250_em485_supported;

/* MCR <-> TIOCM conversion */
static inline int serial8250_TIOCM_to_MCR(int tiocm)
{
	int mcr = 0;

	if (tiocm & TIOCM_RTS)
		mcr |= UART_MCR_RTS;
	if (tiocm & TIOCM_DTR)
		mcr |= UART_MCR_DTR;
	if (tiocm & TIOCM_OUT1)
		mcr |= UART_MCR_OUT1;
	if (tiocm & TIOCM_OUT2)
		mcr |= UART_MCR_OUT2;
	if (tiocm & TIOCM_LOOP)
		mcr |= UART_MCR_LOOP;

	return mcr;
}

static inline int serial8250_MCR_to_TIOCM(int mcr)
{
	int tiocm = 0;

	if (mcr & UART_MCR_RTS)
		tiocm |= TIOCM_RTS;
	if (mcr & UART_MCR_DTR)
		tiocm |= TIOCM_DTR;
	if (mcr & UART_MCR_OUT1)
		tiocm |= TIOCM_OUT1;
	if (mcr & UART_MCR_OUT2)
		tiocm |= TIOCM_OUT2;
	if (mcr & UART_MCR_LOOP)
		tiocm |= TIOCM_LOOP;

	return tiocm;
}

/* MSR <-> TIOCM conversion */
static inline int serial8250_MSR_to_TIOCM(int msr)
{
	int tiocm = 0;

	if (msr & UART_MSR_DCD)
		tiocm |= TIOCM_CAR;
	if (msr & UART_MSR_RI)
		tiocm |= TIOCM_RNG;
	if (msr & UART_MSR_DSR)
		tiocm |= TIOCM_DSR;
	if (msr & UART_MSR_CTS)
		tiocm |= TIOCM_CTS;

	return tiocm;
}

static inline void serial8250_out_MCR(struct uart_8250_port *up, int value)
{
	serial_out(up, UART_MCR, value);

	if (up->gpios)
		mctrl_gpio_set(up->gpios, serial8250_MCR_to_TIOCM(value));
}

static inline int serial8250_in_MCR(struct uart_8250_port *up)
{
	int mctrl;

	mctrl = serial_in(up, UART_MCR);

	if (up->gpios) {
		unsigned int mctrl_gpio = 0;

		mctrl_gpio = mctrl_gpio_get_outputs(up->gpios, &mctrl_gpio);
		mctrl |= serial8250_TIOCM_to_MCR(mctrl_gpio);
	}

	return mctrl;
}

#ifdef CONFIG_SERIAL_8250_PNP
int serial8250_pnp_init(void);
void serial8250_pnp_exit(void);
#else
static inline int serial8250_pnp_init(void) { return 0; }
static inline void serial8250_pnp_exit(void) { }
#endif

#ifdef CONFIG_SERIAL_8250_RSA
void univ8250_rsa_support(struct uart_ops *ops, const struct uart_ops *core_ops);
void rsa_enable(struct uart_8250_port *up);
void rsa_disable(struct uart_8250_port *up);
void rsa_autoconfig(struct uart_8250_port *up);
void rsa_reset(struct uart_8250_port *up);
#else
static inline void univ8250_rsa_support(struct uart_ops *ops, const struct uart_ops *core_ops) { }
static inline void rsa_enable(struct uart_8250_port *up) {}
static inline void rsa_disable(struct uart_8250_port *up) {}
static inline void rsa_autoconfig(struct uart_8250_port *up) {}
static inline void rsa_reset(struct uart_8250_port *up) {}
#endif

#ifdef CONFIG_SERIAL_8250_FINTEK
int fintek_8250_probe(struct uart_8250_port *uart);
#else
static inline int fintek_8250_probe(struct uart_8250_port *uart) { return 0; }
#endif

#ifdef CONFIG_ARCH_OMAP1
#include <linux/soc/ti/omap1-soc.h>
static inline int is_omap1_8250(struct uart_8250_port *pt)
{
	int res;

	switch (pt->port.mapbase) {
	case OMAP1_UART1_BASE:
	case OMAP1_UART2_BASE:
	case OMAP1_UART3_BASE:
		res = 1;
		break;
	default:
		res = 0;
		break;
	}

	return res;
}

static inline int is_omap1510_8250(struct uart_8250_port *pt)
{
	if (!cpu_is_omap1510())
		return 0;

	return is_omap1_8250(pt);
}
#else
static inline int is_omap1_8250(struct uart_8250_port *pt)
{
	return 0;
}
static inline int is_omap1510_8250(struct uart_8250_port *pt)
{
	return 0;
}
#endif

#ifdef CONFIG_SERIAL_8250_DMA
extern int serial8250_tx_dma(struct uart_8250_port *);
extern void serial8250_tx_dma_flush(struct uart_8250_port *);
extern int serial8250_rx_dma(struct uart_8250_port *);
extern void serial8250_rx_dma_flush(struct uart_8250_port *);
extern int serial8250_request_dma(struct uart_8250_port *);
extern void serial8250_release_dma(struct uart_8250_port *);

static inline void serial8250_do_prepare_tx_dma(struct uart_8250_port *p)
{
	struct uart_8250_dma *dma = p->dma;

	if (dma->prepare_tx_dma)
		dma->prepare_tx_dma(p);
}

static inline void serial8250_do_prepare_rx_dma(struct uart_8250_port *p)
{
	struct uart_8250_dma *dma = p->dma;

	if (dma->prepare_rx_dma)
		dma->prepare_rx_dma(p);
}

static inline bool serial8250_tx_dma_running(struct uart_8250_port *p)
{
	struct uart_8250_dma *dma = p->dma;

	return dma && dma->tx_running;
}

static inline void serial8250_tx_dma_pause(struct uart_8250_port *p)
{
	struct uart_8250_dma *dma = p->dma;

	if (!dma->tx_running)
		return;

	dmaengine_pause(dma->txchan);
}

static inline void serial8250_tx_dma_resume(struct uart_8250_port *p)
{
	struct uart_8250_dma *dma = p->dma;

	if (!dma->tx_running)
		return;

	dmaengine_resume(dma->txchan);
}
#else
static inline int serial8250_tx_dma(struct uart_8250_port *p)
{
	return -1;
}
static inline void serial8250_tx_dma_flush(struct uart_8250_port *p) { }
static inline int serial8250_rx_dma(struct uart_8250_port *p)
{
	return -1;
}
static inline void serial8250_rx_dma_flush(struct uart_8250_port *p) { }
static inline int serial8250_request_dma(struct uart_8250_port *p)
{
	return -1;
}
static inline void serial8250_release_dma(struct uart_8250_port *p) { }

static inline bool serial8250_tx_dma_running(struct uart_8250_port *p)
{
	return false;
}

static inline void serial8250_tx_dma_pause(struct uart_8250_port *p) { }
static inline void serial8250_tx_dma_resume(struct uart_8250_port *p) { }
#endif

static inline int ns16550a_goto_highspeed(struct uart_8250_port *up)
{
	unsigned char status;

	status = serial_in(up, 0x04); /* EXCR2 */
#define PRESL(x) ((x) & 0x30)
	if (PRESL(status) == 0x10) {
		/* already in high speed mode */
		return 0;
	} else {
		status &= ~0xB0; /* Disable LOCK, mask out PRESL[01] */
		status |= 0x10;  /* 1.625 divisor for baud_base --> 921600 */
		serial_out(up, 0x04, status);
	}
	return 1;
}

static inline int serial_index(struct uart_port *port)
{
	return port->minor - 64;
}
