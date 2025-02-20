/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Renesas RZ/V2H(P) Interrupt Control Unit (ICU)
 *
 * Copyright (C) 2025 Renesas Electronics Corporation.
 */

#ifndef __LINUX_IRQ_RENESAS_RZV2H
#define __LINUX_IRQ_RENESAS_RZV2H

#include <linux/platform_device.h>

#define RZV2H_ICU_DMAC_REQ_NO_DEFAULT		0x3ff
#define RZV2H_ICU_DMAC_ACK_NO_DEFAULT		0x7f
#define RZV2H_ICU_DMAC_REQ_NO_MIN_FIX_OUTPUT	0x1b5
#define RZV2H_ICU_DMAC_ACK_NO_MIN_FIX_OUTPUT	0x50

#ifdef CONFIG_RENESAS_RZV2H_ICU
void rzv2h_icu_register_dma_req_ack(struct platform_device *icu_dev, u8 dmac_index, u8 dmac_channel,
				    u16 req_no, u8 ack_no);
#else
static inline void rzv2h_icu_register_dma_req_ack(struct platform_device *icu_dev, u8 dmac_index,
						  u8 dmac_channel, u16 req_no, u8 ack_no) { }
#endif

#endif
