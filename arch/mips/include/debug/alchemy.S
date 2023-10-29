/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023, Jiaxun Yang <jiaxun.yang@flygoat.com>
 * MIPS Low level debug include file for Au1xxx UART
 * Dereived from drivers/tty/serial/8250/8250_rt288x.c
 */

#include <asm/addrspace.h>
#include <asm/asm.h>
#include <linux/serial_reg.h>

#define DEBUG_LL_UART

#define UART_BASE	CKSEG1ADDR(CONFIG_DEBUG_UART_PHYS)

#define UART_TX_OFS	(1 << 2)
#define UART_LSR_OFS	(7 << 2)

# define UART_L		lw
# define UART_S		sw

		.macro	addruart,rd,rx
		PTR_LA	\rd, UART_BASE
		.endm

		.macro	senduart,rd,rx
		UART_S   \rd, UART_TX_OFS(\rx)
		sync	/* wmb */
		.endm

		.macro	busyuart,rd,rx
1002:
		UART_L	\rd, UART_LSR_OFS(\rx)
		andi	\rd, \rd, (UART_LSR_TEMT | UART_LSR_THRE)
		xori	\rd, (UART_LSR_TEMT | UART_LSR_THRE)
		sync	/* cpu_relax */
		bnez	\rd, 1002b
		.endm

		.macro	waituarttxrdy,rd,rx
		busyuart \rd, \rx
		.endm

		/* Au1xxx has no MSR */
		.macro	waituartcts,rd,rx
		.endm
