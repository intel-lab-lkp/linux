/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright Advanced Micro Devices, Inc. */
#ifndef DMA_SDXI_RING_H
#define DMA_SDXI_RING_H

#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/range.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <asm/barrier.h>
#include <asm/byteorder.h>
#include <asm/div64.h>
#include <asm/rwonce.h>

#include "hw.h"

/*
 * struct sdxi_ring_state - Descriptor ring management.
 *
 * @lock: Guards *read_index_ptr (RO), *write_index_ptr (RW),
 *   write_index (RW). *read_index is incremented by hw.
 * @write_index: Cached write index value, minimizes dereferences in
 *   critical sections.
 * @write_index_ptr: Location of the architected write index shared with
 *   the SDXI implementation.
 * @read_index_ptr: Location of the architected read index shared with
 *   the SDXI implementation.
 * @entries: Number of entries in the ring.
 * @entry: The descriptor ring itself, shared with the SDXI implementation.
 * @wqh: Pending reservations.
 */
struct sdxi_ring_state {
	spinlock_t lock;
	u64 write_index; /* Cache current value of write index. */
	__le64 *write_index_ptr;
	const __le64 *read_index_ptr;
	u32 entries;
	struct sdxi_desc *entry;
	wait_queue_head_t wqh;
};

/*
 * Ring reservation and iteration state.
 */
struct sdxi_ring_resv {
	const struct sdxi_ring_state *rs;
	struct range range;
	u64 iter;
};

void sdxi_ring_state_init(struct sdxi_ring_state *ring, const __le64 *read_index,
			  __le64 *write_index, u32 entries,
			  struct sdxi_desc descs[static SZ_1K]);
void sdxi_ring_wake_up(struct sdxi_ring_state *rs);
int sdxi_ring_reserve(struct sdxi_ring_state *ring, size_t nr,
		      struct sdxi_ring_resv *resv);
int sdxi_ring_try_reserve(struct sdxi_ring_state *ring, size_t nr,
			  struct sdxi_ring_resv *resv);
struct sdxi_desc *sdxi_ring_resv_next(struct sdxi_ring_resv *resv);

/* Reset reservation's internal iterator. */
static inline void sdxi_ring_resv_reset(struct sdxi_ring_resv *resv)
{
	resv->iter = resv->range.start;
}

/*
 * Return the value that should be written to the doorbell after
 * serializing descriptors for this reservation, i.e. the value of the
 * write index after obtaining the reservation.
 */
static inline u64 sdxi_ring_resv_dbval(const struct sdxi_ring_resv *resv)
{
	return resv->range.end + 1;
}

#define sdxi_ring_resv_foreach(resv_, desc_)			\
	for (sdxi_ring_resv_reset(resv_),			\
	     desc_ = sdxi_ring_resv_next(resv_);		\
	     desc_;						\
	     desc_ = sdxi_ring_resv_next(resv_))

#endif /* DMA_SDXI_RING_H */
