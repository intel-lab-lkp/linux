/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, The Storage Networking Industry Association. */
#ifndef DMA_SDXI_ENQUEUE_H
#define DMA_SDXI_ENQUEUE_H

#include <linux/types.h>

int sdxi_enqueue(const __le64 *enq_entries,  /* Ptr to entries to enqueue */
		 u64 enq_num,  /* Number of entries to enqueue */
		 __le64 *ring_base,  /* Ptr to ring location */
		 u64 ring_size,  /* (Ring Size in bytes)/64 */
		 __le64 const volatile * const Read_Index,  /* Ptr to Read_Index location */
		 __le64 volatile * const Write_Index,  /* Ptr to Write_Index location */
		 __le64 __iomem *Door_Bell);  /* Ptr to Ring Doorbell location */

#endif /* DMA_SDXI_ENQUEUE_H */
