/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _NET_CORE_SKBUFF_SPLICE_H
#define _NET_CORE_SKBUFF_SPLICE_H

#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/types.h>

struct skb_splice_frag {
	u64 anchor;
	unsigned int offset;
	unsigned int len;
};

/*
 * Use page-aligned windows of INT_MAX + 1 bytes.  The anchor is a byte
 * position within the folio, not a pointer; callers have the live folio.
 * Every offset and append length fits the int arguments of the fragment
 * helpers, and offset + length fits u32 without wrapping.  Consecutive
 * calls use the same anchor until a representational boundary is reached.
 */
static inline struct skb_splice_frag skb_splice_frag_normalize(u64 pos, size_t len)
{
	struct skb_splice_frag range = {
		.anchor = pos & ~(u64)INT_MAX,
		.offset = pos & INT_MAX,
	};
	u64 space = (u64)INT_MAX + 1 - range.offset;

	range.len = min_t(u64, len, min_t(u64, space, INT_MAX));
	return range;
}

#endif /* _NET_CORE_SKBUFF_SPLICE_H */
