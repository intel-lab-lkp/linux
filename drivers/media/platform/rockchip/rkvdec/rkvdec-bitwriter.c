// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Video Decoder bit writer
 *
 * Copyright (C) 2026 Collabora, Ltd.
 *      Detlev Casanova <detlev.casanova@collabora.com>
 * Copyright (C) 2019 Collabora, Ltd.
 *	Boris Brezillon <boris.brezillon@collabora.com>
 */

#include <linux/types.h>
#include <linux/bits.h>

#include "rkvdec-bitwriter.h"

void rkvdec_set_bw_field(u32 *buf, struct rkvdec_bw_field field, u32 value)
{
	u8 bit = field.offset % 32;
	u16 word = field.offset / 32;
	u64 mask = GENMASK_ULL(bit + field.len - 1, bit);
	u64 val = ((u64)value << bit) & mask;

	buf[word] &= ~mask;
	buf[word] |= val;
	if (bit + field.len > 32) {
		buf[word + 1] &= ~(mask >> 32);
		buf[word + 1] |= val >> 32;
	}
}

