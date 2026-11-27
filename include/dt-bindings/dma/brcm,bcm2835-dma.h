/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Flags of the DMA specifier of the BCM2835 DMA controller. They are ORed
 * with the DREQ number in the single cell of the specifier.
 */

#ifndef _DT_BINDINGS_DMA_BRCM_BCM2835_DMA_H
#define _DT_BINDINGS_DMA_BRCM_BCM2835_DMA_H

/* AXI priority (0-15) of the channel */
#define BCM2835_DMA_FLAG_PRIORITY(x)		(((x) & 0xf) << 16)
/* AXI panic priority (0-15) of the channel */
#define BCM2835_DMA_FLAG_PANIC_PRIORITY(x)	(((x) & 0xf) << 20)
/* Use wide (128-bit) source reads */
#define BCM2835_DMA_FLAG_WIDE_SOURCE		(1 << 24)
/* Use wide (128-bit) destination writes */
#define BCM2835_DMA_FLAG_WIDE_DEST		(1 << 25)
/* Do not wait for the AXI write responses */
#define BCM2835_DMA_FLAG_NO_WAIT_RESP		(1 << 27)
/* Wait for the outstanding writes before signalling completion */
#define BCM2835_DMA_FLAG_WAIT_FOR_WRITES	(1 << 28)
/* Do not pause the channel when the debug signal is asserted */
#define BCM2835_DMA_FLAG_DIS_DEBUG		(1 << 29)
/* Use multi-beat bursts */
#define BCM2835_DMA_FLAG_BURST			(1 << 30)

#endif /* _DT_BINDINGS_DMA_BRCM_BCM2835_DMA_H */
