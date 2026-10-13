/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Debugging macro include header for the Cortina Systems CS75xx UART
 */

#define CS75XX_UART_TXDAT		0x10
#define CS75XX_UART_INFO		0x18
#define CS75XX_UART_INFO_TX_FULL	(1 << 2)
#define CS75XX_UART_INFO_TX_EMPTY	(1 << 3)

		.macro	addruart, rp, rv, tmp
		ldr	\rp, =CONFIG_DEBUG_UART_PHYS
		ldr	\rv, =CONFIG_DEBUG_UART_VIRT
		.endm

		.macro	senduart, rd, rx
		ARM_BE8(rev \rd, \rd)
		str	\rd, [\rx, #CS75XX_UART_TXDAT]
		ARM_BE8(rev \rd, \rd)
		.endm

		.macro	waituartcts, rd, rx
		.endm

		.macro	waituarttxrdy, rd, rx
1001:		ldr	\rd, [\rx, #CS75XX_UART_INFO]
		ARM_BE8(rev \rd, \rd)
		tst	\rd, #CS75XX_UART_INFO_TX_FULL
		bne	1001b
		.endm

		.macro	busyuart, rd, rx
1002:		ldr	\rd, [\rx, #CS75XX_UART_INFO]
		ARM_BE8(rev \rd, \rd)
		tst	\rd, #CS75XX_UART_INFO_TX_EMPTY
		beq	1002b
		.endm
