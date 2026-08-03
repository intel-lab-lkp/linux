/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Copyright(c) 2018 - 2020 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _HFI2_MSIX_H
#define _HFI2_MSIX_H

#include "hfi2.h"

enum irq_type { IRQ_SDMA, IRQ_RCVCTXT, IRQ_NETDEVCTXT, IRQ_GENERAL, IRQ_OTHER };

/* MSIx interface */
int hfi2_msix_initialize(struct hfi2_devdata *dd);
int hfi2_msix_early_request_irqs(struct hfi2_devdata *dd);
int hfi2_msix_request_irqs(struct hfi2_devdata *dd);
void hfi2_msix_clean_up_interrupts(struct hfi2_devdata *dd);
void hfi2_msix_shut_down_interrupts(struct hfi2_devdata *dd, bool keep_general);
int hfi2_msix_request_general_irq(struct hfi2_devdata *dd);
int hfi2_msix_request_rcd_irq(struct hfi2_ctxtdata *rcd);
int hfi2_msix_request_sdma_irq(struct sdma_engine *sde);
void hfi2_msix_free_irq(struct hfi2_devdata *dd, u8 msix_intr);
int hfi2_msix_request_irq_remap(struct hfi2_devdata *dd, u16 ctxt,
			   enum irq_type type, int src, irq_handler_t handler,
			   irq_handler_t thread, void *arg, const char *name);

/* Netdev interface */
void hfi2_msix_netdev_synchronize_irq(struct hfi2_pportdata *ppd);
int hfi2_msix_netdev_request_rcd_irq(struct hfi2_ctxtdata *rcd);

#endif
