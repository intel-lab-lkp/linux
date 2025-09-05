/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2025 Advanced Micro Devices, Inc. */

#ifndef DMA_SDXI_DMA_H
#define DMA_SDXI_DMA_H

struct sdxi_cxt;

int sdxi_dma_register(struct sdxi_cxt *dma_cxt);
void sdxi_dma_unregister(struct sdxi_cxt *dma_cxt);

#endif /* DMA_SDXI_DMA_H */
