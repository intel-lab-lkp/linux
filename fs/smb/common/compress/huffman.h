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
#include <linux/sizes.h>

#define HUFF_TABLE_SIZE		256
#define HUFF_BLOCK_SIZE		SZ_64K

/*
 * LZ77-Huffman metadata is Huffman table (256 bytes) at the beginning of every 64k block, so
 * compute allocation size considering the worst-case scenarios (fully uncompressible blocks).
 */
static __always_inline u32 smb_huff_compressed_alloc_size(const u32 size)
{
	const u32 nblocks = DIV_ROUND_UP(size, HUFF_BLOCK_SIZE);

	return nblocks * (HUFF_TABLE_SIZE + HUFF_BLOCK_SIZE);
}

int smb_huff_compress(const void *src, const u32 slen, void *dst, u32 *dlen);
int smb_huff_decompress(const void *src, const u32 slen, void *dst, const u32 dlen);
#endif /* _SMB_COMPRESS_LZ77_HUFF_H */
