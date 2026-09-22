/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Loongson-2 SoC DMA Mux dt-bindings
 *
 * DMA specifier format:
 *   dmas = <&dma_mux <request> <channel> <flags>>;
 *
 * The <channel> cell selects a physical CMC DMA channel (0-7)
 * For peripherals using channel pairs (UART, I2C, SPI2/3, I2S),
 * the channel number selects the pair: 0/1 -> pair 0, 2/3 -> pair 1,
 * 4/5 -> pair 2, 6/7 -> pair 3.
 *
 * Copyright (C) 2026 Loongson Technology Corporation Limited
 */

#ifndef _DT_BINDINGS_DMA_LOONGSON_LS2K_DMAMUX_H
#define _DT_BINDINGS_DMA_LOONGSON_LS2K_DMAMUX_H

/* UART controllers */
#define LS2K0300_DMA_UART0		0
#define LS2K0300_DMA_UART1		1
#define LS2K0300_DMA_UART2		2
#define LS2K0300_DMA_UART3		3
#define LS2K0300_DMA_UART4		4
#define LS2K0300_DMA_UART5		5
#define LS2K0300_DMA_UART6		6
#define LS2K0300_DMA_UART7		7
#define LS2K0300_DMA_UART8		8
#define LS2K0300_DMA_UART9		9

/* I2C controllers */
#define LS2K0300_DMA_I2C0		10
#define LS2K0300_DMA_I2C1		11
#define LS2K0300_DMA_I2C2		12
#define LS2K0300_DMA_I2C3		13

/* SPI controllers (IO mode) */
#define LS2K0300_DMA_SPI2		14
#define LS2K0300_DMA_SPI3		15

/* I2S controller */
#define LS2K0300_DMA_I2S		16

/* ADC controller */
#define LS2K0300_DMA_ADC		17

/* CAN-FD controllers */
#define LS2K0300_DMA_CAN0		18
#define LS2K0300_DMA_CAN1		19
#define LS2K0300_DMA_CAN2		20
#define LS2K0300_DMA_CAN3		21

/* Total number of 2K0300 DMA requests */
#define LS2K0300_DMA_REQ_MAX		22

#endif /* _DT_BINDINGS_DMA_LOONGSON_LS2K_DMAMUX_H */
