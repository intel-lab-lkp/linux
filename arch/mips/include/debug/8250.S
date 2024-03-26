/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023, Jiaxun Yang <jiaxun.yang@flygoat.com>
 * MIPS Low level debug include file for 8250 UART
 */

#include <asm/addrspace.h>
#include <asm/asm.h>
#include <linux/serial_reg.h>

#define DEBUG_LL_UART

#define UART_BASE	CKSEG1ADDR(CONFIG_DEBUG_UART_PHYS)

#define UART_TX_OFS	(UART_TX  << CONFIG_DEBUG_UART_8250_SHIFT)
#define UART_LSR_OFS	(UART_LSR << CONFIG_DEBUG_UART_8250_SHIFT)
#define UART_MSR_OFS	(UART_MSR << CONFIG_DEBUG_UART_8250_SHIFT)

#if CONFIG_DEBUG_UART_8250_WIDTH == 1
# define UART_L		lb
# define UART_S		sb
#elif CONFIG_DEBUG_UART_8250_WIDTH == 2
# define UART_L		lh
# define UART_S		sh
#elif CONFIG_DEBUG_UART_8250_WIDTH == 4
# define UART_L		lw
# define UART_S		sw
#elif defined(CONFIG_64BIT) && CONFIG_DEBUG_UART_8250_WIDTH == 8
# define UART_L		ld
# define UART_S		sd
#else
# define UART_L		lb
# define UART_S		sb
#endif

		.macro	addruart,rd,rx
		PTR_LA	\rd, UART_BASE
		.endm

		.macro	senduart,rd,rx
		UART_S   \rd, UART_TX_OFS(\rx)
		.endm

		.macro	busyuart,rd,rx
1002:
		UART_L	\rd, UART_LSR_OFS(\rx)
		andi	\rd, \rd, (UART_LSR_TEMT | UART_LSR_THRE)
		xori	\rd, (UART_LSR_TEMT | UART_LSR_THRE)
		bnez	\rd, 1002b
		.endm

		.macro	waituarttxrdy,rd,rx
		.endm

		.macro	waituartcts,rd,rx
1001:
		UART_L	\rd, UART_MSR_OFS(\rx)
		andi	\rd, UART_MSR_CTS
		beqz	\rd, 1001b
		.endm
