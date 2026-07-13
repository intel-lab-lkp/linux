/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026, SUSE LLC
 *
 * Authors: Enzo Matsumiya <ematsumiya@suse.de>
 *
 * Implementation of the LZ77+Huffman compression algorithm, as per MS-XCA spec.
 */
#ifndef _SMB_COMPRESS_LZ77_HUFF_H
#define _SMB_COMPRESS_LZ77_HUFF_H

#include <linux/kernel.h>

/*
 * LZ77-Huffman metadata is Huffman table (256 bytes) at the beginning of every 64k block, so:
 *
 * metadata = ((@size / 64k) * 256), or simplified (@size >> 8)
 */
static __always_inline u32 smb_huff_compressed_alloc_size(const u32 size)
{
	return size + (size >> 8);
}

int smb_huff_compress(const void *src, const u32 slen, void *dst, u32 *dlen);
int smb_huff_decompress(const void *src, const u32 slen, void *dst, const u32 dlen);
#endif /* _SMB_COMPRESS_LZ77_HUFF_H */
