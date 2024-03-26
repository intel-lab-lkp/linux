/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023, Jiaxun Yang <jiaxun.yang@flygoat.com>
 * MIPS Low level debug include file for ar933x UART
 */

#include <asm/addrspace.h>
#include <asm/asm.h>
#include <asm/mach-ath79/ar933x_uart.h>

#define DEBUG_LL_UART

#define UART_BASE	CKSEG1ADDR(CONFIG_DEBUG_UART_PHYS)

# define UART_L		lw
# define UART_S		sw

		.macro	addruart,rd,rx
		PTR_LA	\rd, UART_BASE
		.endm

		.macro	senduart,rd,rx
		UART_S   \rd, AR933X_UART_DATA_REG(\rx)
		.endm

        /* CTS and RDY are handled by AR933X_UART_DATA_TX_CSR as well */
		.macro	busyuart,rd,rx
1002:
		UART_L	\rd, AR933X_UART_DATA_REG(\rx)
		andi	\rd, \rd, (AR933X_UART_DATA_TX_CSR)
		xori	\rd, (AR933X_UART_DATA_TX_CSR)
		bnez	\rd, 1002b
		.endm

		.macro	waituarttxrdy,rd,rx
        busyuart \rd, \rx
		.endm

		.macro	waituartcts,rd,rx
        busyuart \rd, \rx
		.endm
