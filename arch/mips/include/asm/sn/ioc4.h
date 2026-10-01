/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IOC4 -- SGI's I/O ASIC on Tezro/Origin 350-class boards.
 *
 * Copyright (C) 2003-2006 Silicon Graphics, Inc.
 */
#ifndef MIPS_SN_IOC4_H
#define MIPS_SN_IOC4_H

#include <linux/types.h>

struct ioc4_serialregs {
	u32	sscr;
	u32	stpir;
	u32	stcir;
	u32	srpir;
	u32	srcir;
	u32	srtr;
	u32	shadow;
};

struct ioc4_uartregs {
	u8	i4u_lcr;
	union {
		u8	i4u_iir;	/* read only */
		u8	i4u_fcr;	/* write only */
	};
	union {
		u8	i4u_ier;	/* DLAB == 0 */
		u8	i4u_dlm;	/* DLAB == 1 */
	};
	union {
		u8	i4u_rbr;	/* read only, DLAB == 0 */
		u8	i4u_thr;	/* write only, DLAB == 0 */
		u8	i4u_dll;	/* DLAB == 1 */
	};
	u8	i4u_scr;
	u8	i4u_msr;
	u8	i4u_lsr;
	u8	i4u_mcr;
};

struct ioc4_serioregs {
	u32	km_csr;
	u32	k_rd;
	u32	m_rd;
	u32	k_wd;
	u32	m_wd;
};

/* Register layout of IOC4 in PCI BAR0. */
struct ioc4 {
	u32	pci_err_addr_l;	/* 0x00000 */
	u32	pci_err_addr_h;	/* 0x00004 */
	u32	sio_ir;		/* 0x00008 */
	u32	other_ir;	/* 0x0000c */
	u32	sio_ies;	/* 0x00010 */
	u32	other_ies;	/* 0x00014 */
	u32	sio_iec;	/* 0x00018 */
	u32	other_iec;	/* 0x0001c */
	u32	sio_cr;		/* 0x00020 */
	u32	pad0;		/* 0x00024 */
	u32	int_out;	/* 0x00028 */
	u32	pad1;		/* 0x0002c */
	u32	gpcr_s;		/* 0x00030 */
	u32	gpcr_c;		/* 0x00034 */
	u32	gpdr;		/* 0x00038 */
	u32	pad2;		/* 0x0003c */
	u32	gppr[8];	/* 0x00040 */

	u32	pad3[(0x200 - 0x60) / 4];

	struct ioc4_serioregs	serio;	/* 0x00200, IOC4_KBC_BASE */

	u32	pad4[(0x300 - 0x214) / 4];

	u32	sbbr01_l;	/* 0x00300 */
	u32	sbbr01_h;	/* 0x00304 */
	u32	sbbr23_l;	/* 0x00308 */
	u32	sbbr23_h;	/* 0x0030c */

	struct ioc4_serialregs	port[4];	/* 0x00310 */
	struct ioc4_uartregs	uart[4];	/* 0x00380 */
};

/* Bytebus device 0, where the TOD chip sits. */
#define IOC4_BYTEBUS_DEV0	0x80000L

/* sio_ir carries serial events only, one 8-bit group per port. */
#define IOC4_SIO_IR_TX_MT	0x01	/* TX ring buffer empty */
#define IOC4_SIO_IR_RX_FULL	0x02	/* RX ring buffer full */
#define IOC4_SIO_IR_RX_HIGH	0x04	/* RX high-water exceeded */
#define IOC4_SIO_IR_RX_TIMER	0x08	/* RX timer has triggered */
#define IOC4_SIO_IR_DELTA_DCD	0x10	/* DELTA_DCD seen */
#define IOC4_SIO_IR_DELTA_CTS	0x20	/* DELTA_CTS seen */
#define IOC4_SIO_IR_INT		0x40	/* UART pass-through interrupt */
#define IOC4_SIO_IR_TX_EXPLICIT	0x80	/* TX, MCW, or delay complete */

#define IOC4_SIO_IR_PORT_SHIFT(port)	((port) * 8)
#define IOC4_SIO_IR_PORT_INT(port) \
	(IOC4_SIO_IR_INT << IOC4_SIO_IR_PORT_SHIFT(port))

/* Bit index of IOC4_SIO_IR_INT; DEFINE_RES_IRQ() needs a constant. */
#define IOC4_SIO_IR_INT_BIT		6
#define IOC4_SIO_IR_PORT_INT_BIT(port) \
	(IOC4_SIO_IR_PORT_SHIFT(port) + IOC4_SIO_IR_INT_BIT)

#define IOC4_OTHER_IR_ATA_INT		0x00000001	/* ATA passthru */
#define IOC4_OTHER_IR_ATA_MEMERR	0x00000002	/* ATA PCI error */
#define IOC4_OTHER_IR_S0_MEMERR		0x00000004	/* port 0 PCI error */
#define IOC4_OTHER_IR_S1_MEMERR		0x00000008	/* port 1 PCI error */
#define IOC4_OTHER_IR_S2_MEMERR		0x00000010	/* port 2 PCI error */
#define IOC4_OTHER_IR_S3_MEMERR		0x00000020	/* port 3 PCI error */
/* The IX-brick wires this bit to the PS/2 ports. */
#define IOC4_OTHER_IR_KBD_INT		0x00000040
#define IOC4_OTHER_IR_RT_INT		0x00800000	/* INT_OUT latch */
#define IOC4_OTHER_IR_GEN_INT		0xff000000	/* generic pins */

#define IOC4_OTHER_IR_KBD_INT_BIT	6	/* bit index */

#define IOC4_SSCR_DMA_EN	0x10000000	/* enable ring buffer DMA */

/* GPIO pins 4-7 select RS232 or RS422 mode for the four UARTs. */
#define IOC4_GPCR_UART_MODESEL(port)		(0x10 << (port))
#define IOC4_GPPR_UART_MODESEL_PIN(port)	(4 + (port))

/* The serial core runs off the PCI bus clock, 66MHz in the IX-brick. */
#define IOC4_UARTCLK	66666667

#endif /* MIPS_SN_IOC4_H */
