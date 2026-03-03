/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mod_lineinfo.h - Binary format for per-module source line information
 *
 * This header defines the layout of the .mod_lineinfo section embedded
 * in loadable kernel modules.  It is dual-use: included from both the
 * kernel and the userspace gen_lineinfo tool.
 *
 * Section layout (all values in target-native endianness):
 *
 *   struct mod_lineinfo_header     (24 bytes)
 *   u32 block_addrs[num_blocks]    -- first addr per block, for binary search
 *   u32 block_offsets[num_blocks]  -- byte offset into compressed data stream
 *   u8  data[data_size]            -- ULEB128 delta-compressed entries
 *   u32 file_offsets[num_files]    -- byte offset into filenames[]
 *   char filenames[filenames_size] -- concatenated NUL-terminated strings
 *
 * Compressed stream format (per block of LINEINFO_BLOCK_ENTRIES entries):
 *   Entry 0: file_id (ULEB128), line (ULEB128)
 *            addr is in block_addrs[]
 *   Entry 1..N: addr_delta (ULEB128),
 *               file_id_delta (zigzag-encoded ULEB128),
 *               line_delta (zigzag-encoded ULEB128)
 */
#ifndef _LINUX_MOD_LINEINFO_H
#define _LINUX_MOD_LINEINFO_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;
#endif

#define LINEINFO_BLOCK_ENTRIES 64

struct mod_lineinfo_header {
	u32 num_entries;
	u32 num_files;
	u32 filenames_size;	/* total bytes of concatenated filenames */
	u32 num_blocks;
	u32 data_size;		/* total bytes of compressed data stream */
	u32 reserved;		/* padding, must be 0 */
};

/* Offset helpers: compute byte offset from start of section to each array */

static inline u32 mod_lineinfo_block_addrs_off(void)
{
	return sizeof(struct mod_lineinfo_header);
}

static inline u32 mod_lineinfo_block_offsets_off(u32 num_blocks)
{
	return mod_lineinfo_block_addrs_off() + num_blocks * sizeof(u32);
}

static inline u32 mod_lineinfo_data_off(u32 num_blocks)
{
	return mod_lineinfo_block_offsets_off(num_blocks) +
	       num_blocks * sizeof(u32);
}

static inline u32 mod_lineinfo_file_offsets_off(u32 num_blocks, u32 data_size)
{
	return mod_lineinfo_data_off(num_blocks) + data_size;
}

static inline u32 mod_lineinfo_filenames_off(u32 num_blocks, u32 data_size,
					     u32 num_files)
{
	return mod_lineinfo_file_offsets_off(num_blocks, data_size) +
	       num_files * sizeof(u32);
}

/* Zigzag encoding: map signed to unsigned so small magnitudes are small */
static inline u32 zigzag_encode(int32_t v)
{
	return ((u32)v << 1) ^ (u32)(v >> 31);
}

static inline int32_t zigzag_decode(u32 v)
{
	return (int32_t)((v >> 1) ^ -(v & 1));
}

/*
 * Read a ULEB128 varint from a byte stream.
 * Returns the decoded value and advances *pos past the encoded bytes.
 * If *pos would exceed 'end', returns 0 and sets *pos = end (safe for
 * NMI/panic context -- no crash, just a missed annotation).
 */
static inline u32 lineinfo_read_uleb128(const u8 *data, u32 *pos, u32 end)
{
	u32 result = 0;
	unsigned int shift = 0;

	while (*pos < end) {
		u8 byte = data[*pos];
		(*pos)++;
		result |= (u32)(byte & 0x7f) << shift;
		if (!(byte & 0x80))
			return result;
		shift += 7;
		if (shift >= 32) {
			/* Malformed -- skip remaining continuation bytes */
			while (*pos < end && (data[*pos] & 0x80))
				(*pos)++;
			if (*pos < end)
				(*pos)++;
			return result;
		}
	}
	return result;
}

/* Write a ULEB128 varint -- build tool only */
#ifndef __KERNEL__
static inline unsigned int lineinfo_write_uleb128(u8 *buf, u32 value)
{
	unsigned int len = 0;

	do {
		u8 byte = value & 0x7f;

		value >>= 7;
		if (value)
			byte |= 0x80;
		buf[len++] = byte;
	} while (value);
	return len;
}
#endif /* !__KERNEL__ */

#endif /* _LINUX_MOD_LINEINFO_H */
