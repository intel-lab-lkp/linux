// SPDX-License-Identifier: BSD-3-Clause
/*
 *
 * Copyright (c) 2024, The Storage Networking Industry Association.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * * Neither the name of The Storage Networking Industry Association
 * (SNIA) nor the names of its contributors may be used to endorse or
 * promote products derived from this software without specific prior
 * written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <asm/barrier.h>
#include <asm/byteorder.h>
#include <asm/rwonce.h>
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/processor.h>
#include <linux/types.h>

#include "enqueue.h"

/*
 * Code adapted from the "SDXI Descriptor Ring Operation" chapter of
 * the SDXI spec, specifically the example code in "Enqueuing one or
 * more Descriptors."
 */

#define SDXI_DESCR_SIZE 64
#define SDXI_DS_NUM_QW (SDXI_DESCR_SIZE / sizeof(__le64))
#define SDXI_MULTI_PRODUCER 1  /* Define to 0 if single-producer. */

static int update_ring(const __le64 *enq_entries,   /* Ptr to entries to enqueue */
		       u64 enq_num,                 /* Number of entries to enqueue */
		       __le64 *ring_base,           /* Ptr to ring location */
		       u64 ring_size,               /* (Ring Size in bytes)/64 */
		       u64 index)                   /* Starting ring index to update */
{
	for (u64 i = 0; i < enq_num; i++) {
		__le64 *ringp = ring_base + ((index + i) % ring_size) * SDXI_DS_NUM_QW;
		const __le64 *entryp = enq_entries + (i * SDXI_DS_NUM_QW);

		for (u64 j = 1; j < SDXI_DS_NUM_QW; j++)
			*(ringp + j) = *(entryp + j);
	}

	/* Now write the first QW of the new entries to the ring. */
	dma_wmb();
	for (u64 i = 0; i < enq_num; i++) {
		__le64 *ringp = ring_base + ((index + i) % ring_size) * SDXI_DS_NUM_QW;
		const __le64 *entryp = enq_entries + (i * SDXI_DS_NUM_QW);

		*ringp = *entryp;
	}

	return 0;
}

int sdxi_enqueue(const __le64 *enq_entries,                 /* Ptr to entries to enqueue */
		 u64 enq_num,                               /* Number of entries to enqueue */
		 __le64 *ring_base,                         /* Ptr to ring location */
		 u64 ring_size,                             /* (Ring Size in bytes)/64 */
		 __le64 const volatile * const Read_Index,  /* Ptr to Read_Index location */
		 __le64 volatile * const Write_Index,       /* Ptr to Write_Index location */
		 __le64 __iomem *Door_Bell)                 /* Ptr to Ring Doorbell location */
{
	u64 old_write_idx;
	u64 new_idx;

	while (true) {
		u64 read_idx;

		read_idx = le64_to_cpu(READ_ONCE(*Read_Index));
		dma_rmb();  /* Get Read_Index before Write_Index to always get consistent values */
		old_write_idx = le64_to_cpu(READ_ONCE(*Write_Index));

		if (read_idx > old_write_idx) {
			/* Only happens if Write_Index wraps or ring has bad setup */
			return -EIO;
		}

		new_idx = old_write_idx + enq_num;
		if (new_idx - read_idx > ring_size) {
			cpu_relax();
			continue;  /* Not enough free entries, try again */
		}

		if (SDXI_MULTI_PRODUCER) {
			/* Try to atomically update Write_Index. */
			bool success = cmpxchg(Write_Index,
					       cpu_to_le64(old_write_idx),
					       cpu_to_le64(new_idx)) == cpu_to_le64(old_write_idx);
			if (success)
				break;  /* Updated Write_Index, no need to try again. */
		} else {
			/* Single-Producer case */
			WRITE_ONCE(*Write_Index, cpu_to_le64(new_idx));
			dma_wmb();  /* Make the Write_Index update visible before the Door_Bell update. */
			break;  /* Always successful for single-producer */
		}
		/* Couldn"t update Write_Index, try again. */
	}

	/* Write_Index is now advanced. Let's write out entries to the ring. */
	update_ring(enq_entries, enq_num, ring_base, ring_size, old_write_idx);

	/* Door_Bell write required; only needs ordering wrt update of Write_Index. */
	iowrite64(new_idx, Door_Bell);

	return 0;
}
