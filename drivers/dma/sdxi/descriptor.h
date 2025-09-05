/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DMA_SDXI_DESCRIPTOR_H
#define DMA_SDXI_DESCRIPTOR_H

/*
 * Facilities for encoding SDXI descriptors.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <asm/byteorder.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/sizes.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "hw.h"

static inline void sdxi_desc_set_csb(struct sdxi_desc *desc,
				     dma_addr_t addr)
{
	desc->csb_ptr = cpu_to_le64(FIELD_PREP(SDXI_DSC_CSB_PTR, addr >> 5));
}

struct sdxi_cxt_range {
	u16 cxt_start;
	u16 cxt_end;
};

static inline struct sdxi_cxt_range __sdxi_cxt_range(u16 a, u16 b)
{
	return (struct sdxi_cxt_range) {
		.cxt_start = min(a, b),
		.cxt_end   = max(a, b),
	};
}

#define sdxi_cxt_range_1(_id)			\
	({					\
		u16 id = (_id);			\
		__sdxi_cxt_range(id, id);	\
	})

#define sdxi_cxt_range_2(_id1, _id2) __sdxi_cxt_range(_id1, _id2)

#define _sdxi_cxt_range(_1, _2, _fn, ...) _fn

#define sdxi_cxt_range(...)						\
	_sdxi_cxt_range(__VA_ARGS__,					\
			sdxi_cxt_range_2, sdxi_cxt_range_1)(__VA_ARGS__)

struct sdxi_copy {
	dma_addr_t src;
	dma_addr_t dst;
	size_t len;
	u16 src_akey;
	u16 dst_akey;
};

int sdxi_encode_copy(struct sdxi_desc *desc,
		     const struct sdxi_copy *params);

struct sdxi_intr {
	u16 akey;
};

int sdxi_encode_intr(struct sdxi_desc *desc,
		     const struct sdxi_intr *params);

struct sdxi_cxt_start {
	struct sdxi_cxt_range range;
};

int sdxi_encode_cxt_start(struct sdxi_desc *desc,
			  const struct sdxi_cxt_start *params);

struct sdxi_cxt_stop {
	struct sdxi_cxt_range range;
};

int sdxi_encode_cxt_stop(struct sdxi_desc *desc,
			  const struct sdxi_cxt_stop *params);

/*
 * Fields common to all SDXI descriptors in "unpacked" form, for use
 * with pack_fields() and unpack_fields().
 */
struct sdxi_desc_unpacked {
	u64 csb_ptr;
	u16 type;
	u8 subtype;
	bool vl;
	bool se;
	bool fe;
	bool ch;
	bool csr;
	bool rb;
	bool np;
};

void sdxi_desc_unpack(struct sdxi_desc_unpacked *to,
		      const struct sdxi_desc *from);

#endif /* DMA_SDXI_DESCRIPTOR_H */
