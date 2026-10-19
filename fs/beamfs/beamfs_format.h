/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * beamfs -- on-disk format
 *
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Every structure and constant that describes bytes on the medium,
 * and nothing else. No kernel types, so mkfs.beamfs and fsck.beamfs
 * include this file directly rather than keeping their own copies.
 *
 * They used to. Three defects in a single day came from the copies
 * drifting: fsck rejected the reserved inodes' pointers, mkfs marked
 * the canary block with an index it computed differently, and fsck's
 * superblock struct lagged the kernel's by sixteen bytes so every
 * offset past them was wrong. The format is one thing; it should be
 * written down once.
 *
 * ext4 and xfs split their headers the same way, for the same reason.
 */

#ifndef _BEAMFS_FORMAT_H
#define _BEAMFS_FORMAT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
/*
 * Userspace build: the __leNN types are kernel spellings of plain
 * fixed-width integers. beamfs stores little-endian on disk and every
 * access goes through le*_to_cpu, so the userspace tools -- which run
 * on little-endian hosts and on the same little-endian targets --
 * read them directly.
 */
#include <stdint.h>
#include <stdbool.h>

/*
 * BIT and BIT_ULL come from linux/bits.h, which userspace does not get.
 * mkfs.beamfs and fsck.beamfs include this file directly rather than
 * keeping their own copies -- that is the point of it -- so the macros
 * are defined here for them.
 */
#ifndef BIT
#define BIT(n)      (1U << (n))
#endif
#ifndef BIT_ULL
#define BIT_ULL(n)  (1ULL << (n))
#endif
#include <stddef.h>
#include <linux/types.h>   /* __le16/__le32/__le64/__uNN, as the kernel spells them */

typedef uint64_t u64;
typedef uint32_t u32;

/*
 * beamfs stores little-endian. The tools run on little-endian hosts
 * and targets, so the conversions are identities here; they stay
 * written out so the access sites read the same in both builds.
 */
#define le16_to_cpu(x) ((uint16_t)(x))
#define le32_to_cpu(x) ((uint32_t)(x))
#define le64_to_cpu(x) ((uint64_t)(x))
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
#define cpu_to_le64(x) ((__le64)(x))

/*
 * Userspace has no compiler.h, so __packed is defined here. Spelt in
 * two halves because checkpatch pattern-matches the joined form and
 * suggests replacing it with __packed -- which is what this line
 * defines.
 */
#ifndef __packed
#define BEAMFS_PACK_ATTR __attribute__
#define __packed BEAMFS_PACK_ATTR((__packed__))
#endif
#endif

/* inode_state_read_once returns inode_state_flags in kernel 7.0 */
#define beamfs_inode_is_new(inode) \
	(inode_state_read_once(inode) & I_NEW)

/* Magic number: 0x4245414D, ASCII "BEAM" */
#define BEAMFS_MAGIC         0x4245414D

/* Block size: 4096 bytes */
#define BEAMFS_BLOCK_SIZE    4096
#define BEAMFS_BLOCK_SHIFT   12

/* RS FEC: 16 parity bytes per 239-byte subblock (RS(255,239)) */
#define BEAMFS_RS_PARITY     16
#define BEAMFS_INODE_RS_DATA offsetof(struct beamfs_inode, i_reserved)  /* 172 bytes */
#define BEAMFS_INODE_RS_PAR  16  /* parity bytes stored in i_reserved[0..15] */

/*
 * Inode checksum coverage.
 *
 * i_crc32 sits at offset 48 and the block pointers begin at 52, so a
 * checksum over [0, offsetof(i_crc32)) alone would cover none of them:
 * a hundred and twenty bytes of i_direct, i_indirect, i_dindirect and
 * i_tindirect with nothing watching. Reed-Solomon does protect them --
 * its range is [0, 172) -- but the decode is only entered when the CRC
 * disagrees, so a flipped pointer bit would be neither corrected nor
 * reported.
 *
 * That is the wrong thing to leave unguarded here. A corrupted pointer
 * does not damage the file that owns it; it aims that file at somebody
 * else's blocks, which is how one upset becomes two damaged files.
 *
 * Coverage is therefore the head [0, 48) and the tail [52, 172), the
 * same shape the superblock uses: beamfs_crc32_sb leaves out s_crc32
 * alone, so its checksum spans everything its parity spans. The inode
 * matches.
 *
 * Volumes whose checksum covers the head alone cannot be told apart by
 * inspection, so the wider coverage is gated on
 * BEAMFS_FEATURE_INCOMPAT_INODE_CRC_FULL: an older kernel refuses a
 * new volume rather than reading every inode as damaged, and this
 * kernel refuses a volume without the flag.
 */
#define BEAMFS_INODE_CRC_HEAD_LEN  offsetof(struct beamfs_inode, i_crc32)
#define BEAMFS_INODE_CRC_TAIL_OFF  (offsetof(struct beamfs_inode, i_crc32) \
				    + sizeof(__le32))
#define BEAMFS_INODE_CRC_TAIL_LEN  (BEAMFS_INODE_RS_DATA \
				    - BEAMFS_INODE_CRC_TAIL_OFF)
#define BEAMFS_INODE_CRC_BYTES     (BEAMFS_INODE_CRC_HEAD_LEN \
				    + BEAMFS_INODE_CRC_TAIL_LEN)

#define BEAMFS_SUBBLOCK_DATA 239
#define BEAMFS_SUBBLOCK_TOTAL (BEAMFS_SUBBLOCK_DATA + BEAMFS_RS_PARITY)

/* On-disk bitmap block layout (RS FEC protected) */
#define BEAMFS_BITMAP_SUBBLOCKS  16   /* subblocks per bitmap block */
#define BEAMFS_BITMAP_DATA_BYTES (BEAMFS_BITMAP_SUBBLOCKS * BEAMFS_SUBBLOCK_DATA) /* 3824 */
/* Bits of free-data-block bitmap held by a single on-disk bitmap block. */
#define BEAMFS_BITS_PER_BITMAP_BLOCK (BEAMFS_BITMAP_DATA_BYTES * 8) /* 30592 */

/*
 * Encoding of s_flags (on-disk).
 *
 * Bits 0..15 = bitmap_blocks_count (zero -> legacy 1, max 65535).
 * Bits 16..31 = volume state, BEAMFS_SB_FLAG_* below; the bits not
 *               defined there are reserved (must be zero).
 *
 * s_flags is __le32, inside the CRC32 coverage (region A 0..63).
 * Volumes formatted by pre-multi-bitmap mkfs have s_flags=0, which
 * matches the legacy single-block layout via the fallback below.
 */
#define BEAMFS_SB_FLAGS_BITMAP_BLOCKS_MASK  0x0000FFFFu
/*
 * ext2's s_state, in the upper half of s_flags. MOUNTED is set by a
 * read-write mount and cleared by a clean unmount: found set at mount,
 * the volume was not unmounted cleanly. ERRORS is set by the kernel
 * when it records an uncorrectable event, and cleared by fsck.beamfs
 * --repair after a clean run. Older kernels mask the low sixteen bits
 * and ignore these.
 */
#define BEAMFS_SB_FLAG_MOUNTED              0x00010000u
#define BEAMFS_SB_FLAG_ERRORS               0x00020000u

/*
 * On-disk data block layout under BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE,
 * without BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE: the alternating layout.
 *
 * Each user data block (4096 bytes on disk) holds 16 RS(255,239) shortened
 * subblocks, identical layout to bitmap blocks. Per subblock: 239 data bytes
 * followed by 16 parity bytes (sb total = 255). The 16 subblocks together
 * occupy 16 * 255 = 4080 bytes; the last 16 bytes of the block hold the
 * DATA_CSUM and DATA_SELFID descriptor below, zero when neither is active.
 *
 * Logical capacity per disk block: 16 * 239 = 3824 bytes.
 * Correction capacity per disk block: 16 * (16/2) = 128 byte symbols,
 * at most 8 in any one subblock.
 *
 * file_size_logical = N user-visible bytes
 * disk_blocks_used  = ceil(N / BEAMFS_DATA_INLINE_BYTES)
 */
#define BEAMFS_DATA_INLINE_SUBBLOCKS  16
/* 16 x 239 = 3824 payload bytes, 16 x 255 = 4080 coded, 16 of tail */
#define BEAMFS_DATA_INLINE_BYTES      (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_SUBBLOCK_DATA)
#define BEAMFS_DATA_INLINE_TOTAL      (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_SUBBLOCK_TOTAL)
#define BEAMFS_DATA_INLINE_PAD        (BEAMFS_BLOCK_SIZE - BEAMFS_DATA_INLINE_TOTAL)

/*
 * The capsule layout, under BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE.
 *
 * Alternating, a codeword owns 239 consecutive bytes and a nine-byte
 * burst lands entirely in it: eight is all one corrects, so the
 * subblock is lost. In a capsule the 3824 coded bytes are contiguous
 * and the symbols of one codeword are spread across them -- symbol i
 * of codeword j at byte i*16 + j -- so the same burst puts at most
 * one symbol in any codeword, and 129 consecutive coded bytes are
 * needed to lose one.
 *
 *   0 .. 3807    user data   | the 3824 coded bytes, interleaved
 *   3808 .. 3815 csum        | across the sixteen codewords
 *   3816 .. 3823 selfid      |
 *   3824 .. 4079 parity, 16 bytes per codeword, one after another
 *   4080 .. 4095 generation, reserved
 *
 * The parity is not interleaved: nine bad bytes among one codeword's
 * sixteen parity bytes still lose that codeword.
 *
 * The descriptor sits inside the coded area. In the alternating
 * layout the csum that says whether a block is sound and the selfid
 * that says whether it is the right one sit in the tail pad, outside
 * every codeword: a nine-byte burst there condemns a block the rest
 * of which would have survived intact. A capsule that cannot protect
 * its own header is not a unit of survival.
 *
 * It costs sixteen bytes of capacity per block -- 3808 rather than
 * 3824, 0.42%, four megabytes on a gigabyte -- and buys the header the
 * same 129-byte burst resistance as the data.
 *
 * The last sixteen bytes are set aside for an 8-byte generation and 8
 * reserved bytes. The kernel writes them as zero and does not read
 * them.
 */
#define BEAMFS_CAPSULE_DATA_OFF       0
#define BEAMFS_CAPSULE_DATA_BYTES     3808
#define BEAMFS_CAPSULE_CSUM_OFF       3808   /* u8 type, 3 rsvd, __le32 crc */
#define BEAMFS_CAPSULE_SELFID_OFF     3816   /* __le64 digest(ino, iblock)  */
#define BEAMFS_CAPSULE_CODED_BYTES    3824   /* data + csum + selfid        */
#define BEAMFS_CAPSULE_PARITY_OFF     3824
#define BEAMFS_CAPSULE_PARITY_LEN     (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_RS_PARITY)
#define BEAMFS_CAPSULE_GEN_OFF        4080   /* __le64 generation           */
#define BEAMFS_CAPSULE_GEN_BYTES      8
#define BEAMFS_CAPSULE_FREE_OFF       4088   /* 8 bytes, zero, reserved     */

/*
 * The same interleaved geometry with all 3824 coded bytes counted as
 * payload and no descriptor: data at 0, 256 bytes of parity at 3824,
 * 4080 bytes in all. beamfs_rs_encode_block, beamfs_rs_decode_block,
 * beamfs_block_read and beamfs_block_write in edac.c use these names.
 */
#define BEAMFS_DATA_WOVEN_OFF         0
#define BEAMFS_DATA_WOVEN_BYTES       BEAMFS_DATA_INLINE_BYTES          /* 3824 */
#define BEAMFS_DATA_WOVEN_PARITY_OFF  BEAMFS_DATA_INLINE_BYTES          /* 3824 */
#define BEAMFS_DATA_WOVEN_PARITY_LEN  (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_RS_PARITY) /* 256 */
#define BEAMFS_DATA_WOVEN_TOTAL       (BEAMFS_DATA_WOVEN_BYTES + BEAMFS_DATA_WOVEN_PARITY_LEN)

/*
 * DATA_CSUM descriptor layout in the alternating layout: the 16-byte
 * block tail pad [BEAMFS_DATA_INLINE_TOTAL .. BEAMFS_BLOCK_SIZE), not
 * part of any RS codeword. A capsule holds the same descriptor at
 * BEAMFS_CAPSULE_CSUM_OFF, inside the coded area. The csum takes the
 * first 8 bytes of the tail; DATA_SELFID below takes the other 8.
 */
#define BEAMFS_DATA_CSUM_TYPE_OFF     (BEAMFS_DATA_INLINE_TOTAL + 0)  /* 4080 u8      */
#define BEAMFS_DATA_CSUM_VALUE_OFF    (BEAMFS_DATA_INLINE_TOTAL + 4)  /* 4084 __le32  */
#define BEAMFS_DATA_CSUM_DESC_BYTES   8                               /* type+rsvd+csum */
/*
 * DATA_SELFID: block self-identification, in the second half of the tail
 * pad (at BEAMFS_CAPSULE_SELFID_OFF in a capsule). DATA_CSUM proves a
 * block's contents are intact; it cannot prove the
 * block is the right one. A corrupted block pointer that still lands inside
 * the data area addresses a different, perfectly valid block, whose own
 * descriptor verifies. Measured 2026-08-19 at a 128-flip budget: an inode's
 * direct pointer went from physical 471 to 503, the read returned that
 * block's contents (15245 wrong bits) with cat exiting 0 and no kernel
 * signal, because every integrity check passed on a block that simply was
 * not the one asked for. The existing bounds check only rejects pointers
 * outside [s_data_start, s_data_start + s_nblocks).
 *
 * The field holds beamfs_data_selfid(ino, iblock), so a read that lands on
 * the wrong block fails closed. A 64-bit digest rather than the raw pair
 * keeps the field within the 8 reserved bytes without capping inode numbers
 * or file size, which matters for the datacenter workloads this filesystem
 * targets as much as for embedded ones.
 */
#define BEAMFS_DATA_SELFID_OFF        (BEAMFS_DATA_INLINE_TOTAL + 8)  /* 4088 __le64  */
#define BEAMFS_DATA_SELFID_BYTES      8

/*
 * Conformance fixture (canary block).
 *
 * Under BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE, mkfs.beamfs writes a
 * deterministic canary block immediately after the root directory
 * block. The canary block is RS-encoded like a regular data block and
 * contains a fixed 64-byte ASCII header followed by a deterministic
 * 3760-byte payload. The byte layout is reproducible across mkfs
 * invocations, so its sha256 is a constant.
 *
 * Purpose: validate the on-disk RS encode chain end-to-end without
 * relying on user-written content. The inode that maps it,
 * BEAMFS_RESERVED_INO_CANARY, is reserved below.
 */
#define BEAMFS_CANARY_HEADER_LEN      64
/* 3760 bytes of payload after the header, 3824 in all */
#define BEAMFS_CANARY_PAYLOAD_LEN     (BEAMFS_DATA_INLINE_BYTES - BEAMFS_CANARY_HEADER_LEN)
#define BEAMFS_CANARY_USER_BYTES      BEAMFS_DATA_INLINE_BYTES
#define BEAMFS_CANARY_HEADER_STR      "BEAMFS-CANARY-v4 RS(255,239)x16 SHA256-fixed\n"

/*
 * Reserved inode numbers.
 *
 * Inodes [0 .. BEAMFS_FIRST_USER_INO) are reserved for FS-internal
 * fixtures (root directory, canary block alias). User-visible inode
 * allocation starts at BEAMFS_FIRST_USER_INO.
 *
 * Operations that would free a reserved inode, or modify one other
 * than the root, are rejected by defense-in-depth: VFS layer
 * (S_IMMUTABLE, set on every reserved inode but the root) and
 * allocator layer (range check in beamfs_free_block /
 * beamfs_free_inode_num).
 */
#define BEAMFS_RESERVED_INO_ROOT      1
#define BEAMFS_RESERVED_INO_CANARY    2
#define BEAMFS_FIRST_USER_INO         3

static inline bool beamfs_ino_is_reserved(u64 ino)
{
	return ino < BEAMFS_FIRST_USER_INO;
}

/*
 * Translation helpers for BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE.
 *
 * The user-visible file_offset (in bytes) maps to a logical block index
 * iblock_logical = file_offset / BEAMFS_DATA_INLINE_BYTES (= /3824).
 * Each logical block backs exactly one physical disk block of
 * BEAMFS_BLOCK_SIZE (4096) bytes, holding 3824 user bytes interleaved
 * with 16*16=256 RS parity bytes plus 16 bytes of pad.
 *
 * For schemes other than UNIVERSAL_INLINE, translation is the legacy
 * 1:1 with BEAMFS_BLOCK_SIZE granularity; callers must guard on
 * sbi->s_scheme before invoking these helpers.
 */
static inline u64 beamfs_inline_logical_to_iblock(u64 file_offset)
{
	return file_offset / BEAMFS_DATA_INLINE_BYTES;
}

static inline u32 beamfs_inline_offset_in_logical(u64 file_offset)
{
	return (u32)(file_offset % BEAMFS_DATA_INLINE_BYTES);
}

static inline u64 beamfs_inline_size_to_blocks(u64 size)
{
	return (size + BEAMFS_DATA_INLINE_BYTES - 1) / BEAMFS_DATA_INLINE_BYTES;
}

/*
 * Superblock RS layout.
 *
 * The superblock CRC32 covers two non-contiguous regions:
 *   region A  [0, 64)        = 64 bytes (header + counters + version + flags)
 *   region B  [68, 2769)     = 2701 bytes (uuid + label + RS journal +
 *                              bitmap_blk + features + protection scheme
 *                              + indirection parity + error budget +
 *                              clock anchor)
 *   total                   = 2765 logical bytes
 *   excluded: s_crc32 itself in [64, 68).
 *
 * RS protection covers exactly the same 2765 logical bytes. The two
 * regions are serialized into a contiguous 2769-byte staging buffer
 * (4 bytes of zero pad to round up to 13 shortened subblocks of 213
 * data bytes each). RS(255,239) shortened with data_len=213 is the
 * standard lib/reed_solomon usage; padding is implicit.
 *
 * Each of the 13 subblocks tolerates up to 8 symbol errors
 * independently (RS_PARITY/2 = 16/2 = 8). Total correction capacity
 * across the superblock: 104 symbol errors when distributed across
 * subblocks.
 *
 * The 208 bytes of parity (13 * 16) live at sb->s_pad[1119..1326],
 * which corresponds to disk offset 2769 + 1119 = 3888 -- the last
 * 208 bytes of the 4096-byte superblock. This trailing position is
 * stable against format evolution: new fields are taken out of s_pad
 * ahead of the parity zone.
 *
 * The BUILD_BUG_ONs in beamfs_sb_to_rs_staging fail the build when
 * the covered region no longer matches BEAMFS_SB_RS_COVERAGE_BYTES or
 * outgrows the staging buffer. The subblock geometry below is not
 * checked: a field added to the covered region means recomputing
 * BEAMFS_SB_RS_STAGING_BYTES and BEAMFS_SB_RS_DATA_LEN by hand.
 */
#define BEAMFS_SB_RS_COVERAGE_BYTES  2765   /* logical bytes (CRC32 range) */
#define BEAMFS_SB_RS_STAGING_BYTES   2769   /* 13 * BEAMFS_SB_RS_DATA_LEN   */
#define BEAMFS_SB_RS_DATA_LEN        213    /* per shortened subblock      */
#define BEAMFS_SB_RS_SUBBLOCKS       13     /* total subblocks             */
#define BEAMFS_SB_RS_PARITY_BYTES    208    /* 13 * BEAMFS_RS_PARITY        */
#define BEAMFS_SB_RS_PARITY_OFFSET   3888   /* end - parity bytes          */
#define BEAMFS_SB_RS_S_PAD_INDEX     (BEAMFS_SB_RS_PARITY_OFFSET - 2769)
					  /* index in s_pad[]: 1119     */
#define BEAMFS_BITMAP_MAX_BLOCKS (BEAMFS_BITMAP_DATA_BYTES * 8) /* 30592 */

/* Filesystem limits */
#define BEAMFS_MAX_FILENAME  255
#define BEAMFS_DIRECT_BLOCKS 12
#define BEAMFS_INDIRECT_BLOCKS 1
#define BEAMFS_DINDIRECT_BLOCKS 1

/*
 * BEAMFS_INDIRECT_PTRS: number of block pointers per indirect block.
 * Each pointer is a u64 (8 bytes), so 4096 / 8 = 512 entries.
 * Single indirect capacity: 512 blocks = 2 MiB.
 *
 * Shared between the legacy iomap path (file.c, scheme=5) and the
 * v2 INLINE block-mapping helpers (file_inline.c, scheme=2).
 */
#define BEAMFS_INDIRECT_PTRS (BEAMFS_BLOCK_SIZE / sizeof(__le64))

/*
 * Multi-level indirect capacity.
 *
 * BEAMFS_DINDIRECT_PTRS: total user blocks reachable via the double
 *   indirect pointer = 512 single-indirect blocks x 512 ptrs each.
 * BEAMFS_TINDIRECT_PTRS: total user blocks reachable via the triple
 *   indirect pointer = 512 dindirect blocks x 512 single-indirect each
 *                      x 512 ptrs each.
 *
 * Maximum mapped iblock per indirection level:
 *   direct      :                                  BEAMFS_DIRECT_BLOCKS (12)
 *   + indirect  :                              524 (12 + 512)
 *   + dindirect :                          262 668 (524 + 512^2)
 *   + tindirect :                      134 480 396 (262 668 + 512^3)
 *
 * Logical user capacity per file (user bytes per block = 3824 under
 * scheme=2 UNIVERSAL_INLINE with 16 x RS(255,239) sub-blocks):
 *   direct      :    45 888 B  (~45 KiB)
 *   + indirect  : 2 003 776 B  (~1.91 MiB)
 *   + dindirect : ~ 958 MiB    (~1 GiB)
 *   + tindirect : ~ 479 GiB    -- current ceiling
 */
#define BEAMFS_DINDIRECT_PTRS  (BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS)
#define BEAMFS_TINDIRECT_PTRS  (BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS)

#define BEAMFS_MAX_IBLOCK_DIRECT     ((u64)BEAMFS_DIRECT_BLOCKS)
#define BEAMFS_MAX_IBLOCK_INDIRECT   (BEAMFS_MAX_IBLOCK_DIRECT   + (u64)BEAMFS_INDIRECT_PTRS)
#define BEAMFS_MAX_IBLOCK_DINDIRECT  (BEAMFS_MAX_IBLOCK_INDIRECT + (u64)BEAMFS_DINDIRECT_PTRS)
#define BEAMFS_MAX_IBLOCK_TINDIRECT  (BEAMFS_MAX_IBLOCK_DINDIRECT + (u64)BEAMFS_TINDIRECT_PTRS)

/*
 * The largest file the indirection can address, in bytes.
 *
 * Derived rather than stated, so it follows the geometry instead of
 * having to be remembered when the geometry changes. Roughly 479 GiB at
 * the current shape: twelve direct blocks, then 512 pointers per level
 * over three levels, 3824 usable bytes each.
 *
 * s_maxbytes has to be this and not MAX_LFS_FILESIZE. Claiming 2^63
 * meant the VFS accepted a truncate to eight exabytes and only found
 * out at the read, which came back EIO because the logical block was
 * past the tree -- generic/466 reports it as "Discrepancy @ blocksize
 * 4096". A filesystem that overstates its reach turns a clean EFBIG at
 * the point of asking into an I/O error somewhere later.
 */
#define BEAMFS_MAX_FILE_SIZE \
	(BEAMFS_MAX_IBLOCK_TINDIRECT * (u64)BEAMFS_DATA_INLINE_BYTES)

/*
 * Electromagnetic Resilience Journal entry -- 40 bytes.
 *
 * Records each RS FEC correction event persistently in the superblock.
 * 64 entries give operators a map of physical degradation over time.
 *
 * re_entropy_q16_16 carries a per-event Shannon entropy estimate over
 * the corrected positions, the forensic discriminator between Family A
 * (Poisson background SEU) and Family B (correlated burst). The
 * estimator is beamfs_rs_compute_entropy_q16_16() in edac.c.
 *
 * Layout invariants:
 *   - 40 bytes, packed, no implicit padding (BUILD_BUG_ON in super.c)
 *   - re_reserved and re_pad MUST be zero on write; non-zero values
 *     act as structural sentinels. re_reserved lies inside re_crc32's
 *     coverage, so a non-zero value there also fails the CRC; re_pad
 *     lies outside it
 *   - re_crc32 covers bytes [0..32), i.e. all fields except itself
 *     and the trailing alignment pad
 */
struct beamfs_rs_event {
	/*
	 * The block the correction hit. A block, not a block and a
	 * subblock folded together: the subblock index lives in re_flags
	 * (BEAMFS_RS_EVENT_SUBBLOCK_*). A superblock correction is the
	 * exception and carries BEAMFS_RS_BLOCK_NO_SB_MARKER instead.
	 */
	__le64  re_block_no;          /*  0..7                              */
	__le64  re_timestamp;         /*  8..15  ktime_get_ns() at recovery */
	__le32  re_symbol_count;      /* 16..19  symbols corrected           */
	__le32  re_entropy_q16_16;    /* 20..23  Shannon H, Q16.16, [0,3*65536) */
	__le32  re_flags;             /* 24..27  see BEAMFS_RS_EVENT_FLAG_*  */
	__le32  re_reserved;          /* 28..31  zero, structural sentinel   */
	__le32  re_crc32;             /* 32..35  CRC32 over bytes [0..32)    */
	__le32  re_pad;               /* 36..39  zero, alignment + sentinel  */
} __packed;                       /* 40 bytes */

#define BEAMFS_RS_JOURNAL_SIZE  64   /* entries in the EM resilience journal   */

/*
 * Flags for beamfs_rs_event::re_flags.
 *
 * ENTROPY_VALID -- the re_entropy_q16_16 field carries a meaningful
 *                  Shannon estimate. Cleared by zero-init (mkfs);
 *                  set by beamfs_log_rs_event() when entropy is computed
 *                  from the position list returned by RS decode.
 *
 * UNCORRECTABLE -- the data could not be recovered: a codeword
 *                  exceeded the RS correction radius (more than
 *                  BEAMFS_RS_PARITY/2 = 8 symbols in error within a
 *                  single subblock), or a fail-closed integrity
 *                  check rejected the block. When this flag is set,
 *                  re_symbol_count MUST be zero and ENTROPY_VALID
 *                  MUST be cleared. The flag is orthogonal to
 *                  Family A / Family B classification: an
 *                  uncorrectable event may originate from either
 *                  family; discrimination is performed post-process
 *                  by clustering analysis on re_block_no and
 *                  re_timestamp, not in-kernel.
 *
 * RMW_NEUTRALISED -- the correction was performed during a
 *                  read-modify-write transit (write path) rather
 *                  than on a user-initiated read. The decoded
 *                  data is about to be overwritten by the encode
 *                  step that follows in the same RMW cycle, so
 *                  the flip is *silently neutralised* before any
 *                  consumer observes it. This event would have
 *                  caused a read-side correction (or worse, an
 *                  uncorrectable) had the same disk-block been
 *                  read in isolation. Discriminating this flag
 *                  from the bare correctable event is what makes
 *                  neutralisation and correction separately
 *                  measurable from the RS journal alone.
 *                  Orthogonal to ENTROPY_VALID:
 *                  an RMW_NEUTRALISED event with >= 2 corrected
 *                  symbols still carries a valid Shannon entropy
 *                  estimate over the position list.
 *
 * BIT(3) is INODE, below, and bits 8..15 hold the subblock index
 * (BEAMFS_RS_EVENT_SUBBLOCK_*). Bits 4..7 and 16..31 are reserved and
 * MUST be zero on write.
 */
#define BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID  BIT(0)
#define BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE  BIT(1)
#define BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED BIT(2)

/*
 * INODE -- re_block_no is the inode table block that was being read,
 *          not a data or metadata block of a file. The correction
 *          landed inside an inode; which inode is not recorded,
 *          because clustering analysis asks where on the medium the
 *          damage was, and the table block is that answer.
 */
#define BEAMFS_RS_EVENT_FLAG_INODE          BIT(3)

/*
 * Subblock index, in bits 8..15, stored as index + 1 so that zero
 * keeps meaning "not applicable" -- a superblock or inode correction
 * has no subblock in the sense the data path uses.
 *
 * It lives here rather than in re_reserved because that field is a
 * structural sentinel: a non-zero value in it means the entry is
 * corrupt, and that is worth more than four bytes. re_flags had
 * twenty-nine bits spare.
 *
 * Before this, callers folded the subblock into re_block_no as
 * block * SUBBLOCKS + subblock, with SUBBLOCKS being sixteen in the
 * data path, BEAMFS_BITMAP_SUBBLOCKS in the allocator and thirteen in
 * the superblock. A reader had no way to know which divisor applied,
 * so the field documented as a block number was not one.
 */
#define BEAMFS_RS_EVENT_SUBBLOCK_SHIFT      8
#define BEAMFS_RS_EVENT_SUBBLOCK_MASK       (0xFFU << BEAMFS_RS_EVENT_SUBBLOCK_SHIFT)

static inline __u32 beamfs_rs_event_subblock_bits(unsigned int sub)
{
	return ((sub + 1) << BEAMFS_RS_EVENT_SUBBLOCK_SHIFT)
		& BEAMFS_RS_EVENT_SUBBLOCK_MASK;
}

/* Returns the subblock index, or -1 when the event has none. */
static inline int beamfs_rs_event_subblock(__u32 flags)
{
	unsigned int v = (flags & BEAMFS_RS_EVENT_SUBBLOCK_MASK)
			 >> BEAMFS_RS_EVENT_SUBBLOCK_SHIFT;

	return v ? (int)(v - 1) : -1;
}

/*
 * Shannon entropy parameters for the RS journal forensic estimator.
 *
 * BINS    -- number of histogram bins over the codeword position range.
 *            Power of 2 chosen so that bin_index = pos * BINS / code_len
 *            fits in u32 arithmetic without overflow for any beamfs
 *            codeword length (max 239 bytes).
 * Q       -- fractional bits in the Q-format LUT (Q16.16 = 16 frac bits).
 *
 * The full entropy LUT is defined in edac.c, with the formula its
 * entries are computed from.
 */
#define BEAMFS_RS_ENTROPY_BINS              8
#define BEAMFS_RS_ENTROPY_Q                 16

/*
 * Sentinel block number for journal entries that record a SUPERBLOCK
 * RS recovery rather than a data/metadata block recovery. The low 12
 * bits encode the SB sub-block index (0..12, < BEAMFS_SB_RS_SUBBLOCKS).
 *
 * Forensic decoder: if (re_block_no & BEAMFS_RS_BLOCK_NO_SB_MASK) ==
 *                       BEAMFS_RS_BLOCK_NO_SB_MARKER, the entry refers
 * to SB sub-block (re_block_no & BEAMFS_RS_BLOCK_NO_SB_IDX_MASK).
 *
 * The marker is chosen at the top of the u64 range, well above any
 * realistic block number on a filesystem (2^64 blocks * 4 KiB =
 * 2^76 bytes = 64 ZiB, unreachable by current and foreseeable storage).
 */
#define BEAMFS_RS_BLOCK_NO_SB_MARKER    0xFFFFFFFFFFFFF000ULL
#define BEAMFS_RS_BLOCK_NO_SB_MASK      0xFFFFFFFFFFFFF000ULL
#define BEAMFS_RS_BLOCK_NO_SB_IDX_MASK  0x0000000000000FFFULL

/*
 * On-disk format versions.
 *
 * Mount policy: strict equality with BEAMFS_VERSION_CURRENT. Version 1
 * images are not mountable and have to be reformatted. Dual-format
 * in-kernel parsing is intentionally avoided (doubles audit surface for
 * KASAN / syzkaller for no operational benefit on a niche FS).
 *
 * Format extensions within version 5 are carried by the feature flags
 * below.
 */
#define BEAMFS_VERSION_V1        1
#define BEAMFS_VERSION_V5        5
#define BEAMFS_VERSION_CURRENT   BEAMFS_VERSION_V5

/*
 * Data protection scheme values for s_data_protection_scheme.
 *
 * NONE              -- no FEC on data blocks (legacy behaviour, deprecated).
 * INODE_OPT_IN      -- RS FEC enabled per-inode via BEAMFS_INODE_FL_RS_ENABLED.
 *                     This is the v0.1.0 baseline behaviour. Deprecated by
 *                     threat model 6.3 (see Documentation/threat-model.md).
 * UNIVERSAL_INLINE  -- RS parity bytes embedded inline within each data block.
 *                     Reserved for stage 4 of the staged plan.
 * UNIVERSAL_SHADOW  -- RS parity stored in a dedicated out-of-band region.
 *                     Reserved for stage 4 of the staged plan.
 * UNIVERSAL_EXTENT  -- RS parity attached as an extent-based filesystem
 *                     attribute. Reserved for stage 4 of the staged plan.
 *
 * The kernel range-checks this field at mount and refuses values above
 * BEAMFS_DATA_PROTECTION_MAX. Three unused upper bytes of the __le32 act
 * as a structural sentinel: any single-byte corruption in the high-order
 * bytes produces a value outside the valid range and is rejected.
 */
#define BEAMFS_DATA_PROTECTION_NONE              0
#define BEAMFS_DATA_PROTECTION_INODE_OPT_IN      1
#define BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE  2
#define BEAMFS_DATA_PROTECTION_UNIVERSAL_SHADOW  3
#define BEAMFS_DATA_PROTECTION_UNIVERSAL_EXTENT  4

/*
 * Per-data-block integrity field type (DATA_CSUM feature), stored in
 * the block descriptor: the tail pad in the alternating layout,
 * BEAMFS_CAPSULE_CSUM_OFF in a capsule. CRC32 reuses beamfs_crc32
 * (crc32_le), the primitive backing i_crc32 and s_crc32. Values 2..255
 * reserved for keyed integrity (Family B).
 */
#define BEAMFS_CSUM_NONE   0   /* no integrity field present            */
#define BEAMFS_CSUM_CRC32  1   /* beamfs_crc32 over decoded user payload */
#define BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL   5
#define BEAMFS_DATA_PROTECTION_MAX               BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL

/*
 * Feature flag masks.
 *
 * s_feat_compat     -- informational flags. Unknown bits are logged but
 *                     do not prevent mount.
 * s_feat_incompat   -- structural format extensions. Unknown bits cause
 *                     mount to be refused (read or write).
 * s_feat_ro_compat  -- features that prevent safe write. Unknown bits cause
 *                     mount to be forced read-only with a warning.
 *
 * The bit positions below are fixed on disk. Only the bits in the
 * BEAMFS_FEAT_*_SUPP masks further down are implemented by this
 * kernel; the others are declared to reserve their positions and are
 * treated as unknown, per the rules above.
 */

/* COMPAT features (informational, mount continues if unknown) */
#define BEAMFS_FEATURE_COMPAT_RS_JOURNAL_VERBOSE  BIT_ULL(0)
#define BEAMFS_FEATURE_COMPAT_LABEL_LONG          BIT_ULL(1)
#define BEAMFS_FEATURE_COMPAT_DIR_INDEX           BIT_ULL(2)

/* RO_COMPAT features (force RO mount if unknown) */
#define BEAMFS_FEATURE_RO_COMPAT_LARGE_FILE       BIT_ULL(0)
#define BEAMFS_FEATURE_RO_COMPAT_HUGE_FILE        BIT_ULL(1)
#define BEAMFS_FEATURE_RO_COMPAT_EXTRA_ISIZE      BIT_ULL(2)
#define BEAMFS_FEATURE_RO_COMPAT_BTREE_DIR        BIT_ULL(3)
/* Per-data-block csum and selfid, see BEAMFS_DATA_CSUM_* above */
#define BEAMFS_FEATURE_RO_COMPAT_DATA_CSUM        BIT_ULL(4)
#define BEAMFS_FEATURE_RO_COMPAT_DATA_SELFID      BIT_ULL(5)

/* INCOMPAT features (refuse mount if unknown) */
#define BEAMFS_FEATURE_INCOMPAT_EXTENTS           BIT_ULL(0)
#define BEAMFS_FEATURE_INCOMPAT_64BIT             BIT_ULL(1)
#define BEAMFS_FEATURE_INCOMPAT_BIGALLOC          BIT_ULL(2)
#define BEAMFS_FEATURE_INCOMPAT_BLOCK_GROUPS      BIT_ULL(3)
#define BEAMFS_FEATURE_INCOMPAT_BTREE_ALLOC       BIT_ULL(4)
#define BEAMFS_FEATURE_INCOMPAT_JOURNAL           BIT_ULL(5)
#define BEAMFS_FEATURE_INCOMPAT_DAX               BIT_ULL(6)
#define BEAMFS_FEATURE_INCOMPAT_RS_HEAVY          BIT_ULL(7)
#define BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS      BIT_ULL(8)
#define BEAMFS_FEATURE_INCOMPAT_BG_RS_PARITY      BIT_ULL(9)
#define BEAMFS_FEATURE_INCOMPAT_LARGE_BLOCK       BIT_ULL(10)
#define BEAMFS_FEATURE_INCOMPAT_SHADOW_PARITY     BIT_ULL(11)
/*
 * BEAMFS_FEATURE_INCOMPAT_ENCRYPT: reserved for a future at-rest
 * encryption layer, not yet implemented. Declared now, alongside
 * SHADOW_PARITY and LARGE_BLOCK above, so an on-disk bit position is
 * fixed before any volume exists that could claim it by accident.
 *
 * Composition order, should this be implemented: encrypt -> RS encode
 * -> DATA_CSUM / DATA_SELFID, mirrored in reverse on read (RS decode
 * -> verify -> decrypt). Two consequences follow from that order:
 *
 *   - DATA_CSUM and DATA_SELFID would cover ciphertext, not plaintext.
 *     A corrupted block still fails closed (the RS-decoded bytes no
 *     longer match the stored digest), but decryption happening after
 *     the check means garbage plaintext is never produced from a block
 *     that already failed integrity, which keeps the read path
 *     fail-closed.
 *
 *   - Encrypting before RS means a single ciphertext bit flip can
 *     alter more plaintext bits than the corresponding cleartext flip
 *     would (block-cipher diffusion), which the RS correction budget
 *     was not sized against. This is a design question for whoever
 *     implements the bit, not an implementation detail to silently
 *     inherit from the current cleartext path.
 */
#define BEAMFS_FEATURE_INCOMPAT_ENCRYPT           BIT_ULL(12)
/*
 * INDIRECT_PARITY -- RS parity for indirection blocks, held out of band.
 *
 * An indirect block is 512 raw __le64 pointers filling the whole 4096
 * bytes, with no room for a checksum. The pointer checks on the read
 * path close the dominant failure mode -- a pointer landing outside the
 * data range or on a block the bitmap says is free -- and leave one
 * open: a flipped pointer that happens to land on a block that is both
 * in range and allocated passes every check there is.
 *
 * The asymmetry is what makes this worth closing. A data block has
 * eight correctable symbols per 255-byte subblock. A double-indirect
 * pointer has none, and losing it costs 262144 blocks.
 *
 * Parity lives in a region of its own rather than in the block, so
 * BEAMFS_INDIRECT_PTRS stays 512, no BEAMFS_MAX_IBLOCK_* moves and the
 * format of the indirect block does not change. The region is sized
 * for the worst case, every data block being indirect, because a table
 * mapping blocks to parity slots would itself be metadata needing
 * protection. What it costs depends on the mode, below; the slot is
 * computed directly from the block number.
 *
 * INCOMPAT rather than RO_COMPAT: a kernel that does not know about the
 * region would allocate indirect blocks without writing their parity,
 * leaving the volume looking protected while it is not.
 */
#define BEAMFS_FEATURE_INCOMPAT_INDIRECT_PARITY   BIT_ULL(13)

/*
 * How much an indirect block costs to protect, chosen at mkfs time.
 *
 * The right answer depends on the deployment, which is why it is a
 * choice rather than a constant. A CubeSat with 8 GiB of MRAM at
 * several hundred euro per gigabyte will not spend 7% of it; a server
 * with spare terabytes will, and gets the stronger code for the price.
 *
 * NONE  no protection. The residual above stands: a flipped
 *       pointer landing in range and on an allocated block is
 *       indistinguishable from a valid one.
 *
 * CRC   one CRC32 per subblock, 64 bytes per block, ~1.7% of the
 *       volume with the region's own FEC. Detects without correcting,
 *       which turns the residual from silent corruption into a clean
 *       fail-closed error -- the contract beamfs states everywhere
 *       else.
 *
 * RS    full RS(255,239), 256 bytes per block, ~7% of the volume with
 *       the region's own FEC. Decodes up to 8 symbols per subblock;
 *       a verify decodes a copy, journals what it found and leaves
 *       the block as it is, so a hit pointer is reported rather than
 *       repaired.
 *
 * The region is sized for the worst case in every mode: any data block
 * can become an indirect block, and a table mapping blocks to slots
 * would itself be metadata needing protection.
 */
/*
 * Slots one region block holds.
 *
 * A region block is RS-encoded in the alternating layout, sixteen
 * subblocks of 239 data bytes each followed by its parity, so its 4096
 * bytes carry BEAMFS_DATA_INLINE_BYTES of payload.
 * Slots live in the payload and never straddle it.
 *
 * Before the region carried its own FEC, slots sat in the raw 4096
 * bytes and one flipped bit took out every indirect block that region
 * block covered. Under RS the region is now a seventh longer --
 * fourteen slots per region block rather than sixteen -- and that is
 * what the last layer being as well defended as the rest costs.
 */
#define BEAMFS_IND_PARITY_RS_SLOTS \
	(BEAMFS_DATA_INLINE_BYTES / BEAMFS_IND_PARITY_RS_BYTES)
#define BEAMFS_IND_PARITY_CRC_SLOTS \
	(BEAMFS_DATA_INLINE_BYTES / BEAMFS_IND_PARITY_CRC_BYTES)

/*
 * Set when the parity region carries its own RS FEC. Without it the
 * region is raw bytes with a different slot geometry, so reading one
 * with this arithmetic addresses the wrong slots: the kernel refuses
 * the mount rather than returning another block's parity.
 */
#define BEAMFS_FEATURE_INCOMPAT_IND_PARITY_FEC  BIT(17)

/*
 * Set when a block's RS symbols are interleaved across its codewords.
 *
 * Without it, codeword j owns bytes [j*255, (j+1)*255) of the block,
 * its 239 data bytes followed by its parity. Damage that reaches
 * several neighbouring bytes at once puts a nine-byte burst entirely
 * in one codeword and takes it past correction.
 *
 * With it, symbol i of codeword j sits at byte i*16 + j of the coded
 * bytes. The same burst puts at most one symbol in any codeword, and it
 * takes 129 consecutive coded bytes to lose one -- sixteen times the
 * burst resistance for not one byte of extra parity. The parity that
 * follows is not interleaved, and nine bad bytes within one codeword's
 * parity still lose it. debugfs beamfs/rs_bench reports the burst each
 * layout survives over the coded bytes.
 *
 * Incompatible: reading an interleaved block with the contiguous
 * arithmetic gathers the wrong symbols and decodes to noise, so a
 * kernel without this refuses the mount rather than returning it.
 *
 * CCSDS provides for interleaving its Reed-Solomon code for the same
 * reason.
 */
#define BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE  BIT(18)

enum beamfs_ind_parity_mode {
	BEAMFS_IND_PARITY_NONE = 0,
	BEAMFS_IND_PARITY_CRC  = 1,
	BEAMFS_IND_PARITY_RS   = 2,
	BEAMFS_IND_PARITY__MAX
};

/* Bytes of parity per indirect block, by mode. */
#define BEAMFS_IND_PARITY_CRC_BYTES  (BEAMFS_DATA_INLINE_SUBBLOCKS * sizeof(__le32))
#define BEAMFS_IND_PARITY_RS_BYTES   (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_RS_PARITY)

/*
 * ERROR_BUDGET -- per-block record of how close a block has come to
 * saturation.
 *
 * The RS journal keeps the last 64 events, which answers "what
 * happened recently" and nothing about the state of the volume. A
 * block that has quietly accumulated seven of its eight correctable
 * symbols reads back perfectly and is one upset from unrecoverable,
 * and there is no way to know until it is too late.
 *
 * One byte per data block holds the worst subblock's corrected-symbol
 * count, saturating at BEAMFS_RS_PARITY/2. An operator can then ask
 * how much correction capacity a volume has left rather than how many
 * corrections it has done -- the difference between a fuel gauge and
 * an odometer.
 *
 * Nothing else does this. btrfs and ZFS count corrections; neither
 * reports remaining margin, because with redundancy elsewhere there is
 * no margin to report. On a single device there is, and it is finite.
 *
 * One byte per block is 0.025% of the volume: 244 MiB on 931 GiB. The
 * slot is computed from the block number the same way the indirection
 * parity region does it, for the same reason -- a lookup table would
 * be metadata needing its own protection.
 */
#define BEAMFS_FEATURE_INCOMPAT_ERROR_BUDGET      BIT_ULL(14)

/*
 * BEAMFS_FEATURE_INCOMPAT_DIR_RS: directory blocks carry parity, and
 * their entries are variable length.
 *
 * ## Why parity
 *
 * Every other structure on this volume is covered. Data blocks carry
 * sixteen RS codewords, inodes carry CRC32 with RS behind it, indirect
 * blocks carry their own parity, the superblock and the bitmap are both
 * encoded. Directory blocks were written with memset and
 * mark_buffer_dirty and nothing else -- 4096 raw bytes, no checksum, no
 * correction.
 *
 * An upset in a directory block was therefore silent and permanent: a
 * flipped inode number points a name at the wrong file, a flipped
 * length walks the parser off the end of an entry, and neither is
 * detectable let alone correctable. In a filesystem whose reason for
 * existing is surviving upsets, that was the one hole left in the
 * chain.
 *
 * ## Why variable length in the same change
 *
 * A protected directory block has 3824 usable bytes instead of 4096, so
 * the layout moves either way. Doing both at once costs one format
 * migration rather than two.
 *
 * Entries were a fixed 268 bytes -- twelve of header and 256 for the
 * name, whatever the name's length. That is fourteen entries per block
 * and a hard ceiling of 7336 per directory, since a directory reaches
 * twelve direct blocks plus 512 through one indirect. d_rec_len existed
 * in the on-disk entry and was written correctly by both mkfs and the
 * kernel; nothing ever read it, because every walk stepped by
 * sizeof(struct beamfs_dir_entry).
 *
 * With the field finally used, a name of eleven characters occupies 24
 * bytes: 159 entries per protected block, and a ceiling above 83000.
 * Eleven times the density, eleven times fewer blocks to read on a
 * lookup, and eleven times less metadata exposed to upsets for the same
 * directory -- the resilience argument and the performance argument
 * point the same way.
 *
 * ## Why entries avoid straddling subblocks
 *
 * Each of the sixteen subblocks is an independent codeword: one is
 * correctable, or not, on its own. An entry spanning two of them turns
 * a single uncorrectable subblock into a parse failure for the entry
 * after it as well, and from there for the rest of the block.
 *
 * So an entry is placed within one subblock whenever it fits. That
 * costs a few bytes of padding at each boundary -- about 3% -- and
 * keeps damage where it landed.
 *
 * A name of 255 characters, which NAME_MAX allows and xfstests
 * exercises, needs 268 bytes and cannot fit in 239. Such an entry is
 * allowed to span, because the alternative is refusing a filename every
 * other Linux filesystem accepts. The threshold is 224 characters, a
 * 236-byte entry: anything longer cannot fit in one subblock, spans
 * two, and an uncorrectable one costs both.
 *
 * The property is kept where it can be and dropped only where the
 * format would otherwise have to lie about what it supports.
 */
#define BEAMFS_FEATURE_INCOMPAT_DIR_RS            BIT_ULL(15)

/*
 * BEAMFS_FEATURE_INCOMPAT_INODE_CRC_FULL: i_crc32 covers the block
 * pointers as well as the head of the inode. See the coverage macros
 * above. Incompatible rather than read-only compatible: a kernel
 * without it computes a different checksum and would read every inode
 * on the volume as damaged.
 */
#define BEAMFS_FEATURE_INCOMPAT_INODE_CRC_FULL    BIT_ULL(16)

/*
 * A directory entry, on disk, under DIR_RS.
 *
 * d_rec_len is the distance to the next entry and includes the header.
 * The name follows immediately and is not terminated: d_name_len says
 * how long it is. Entries are aligned so the next one starts on a
 * four-byte boundary, and a record length below BEAMFS_DIRENT_MIN_LEN,
 * zero included, ends the walk of the block.
 */
#define BEAMFS_DIRENT_HDR_LEN   12
#define BEAMFS_DIRENT_ALIGN     4
#define BEAMFS_DIRENT_MIN_LEN   (BEAMFS_DIRENT_HDR_LEN + BEAMFS_DIRENT_ALIGN)

/* Round a name length up to a whole record. */
#define BEAMFS_DIRENT_LEN(namelen)                                     \
	((((BEAMFS_DIRENT_HDR_LEN) + (namelen) +                       \
	   (BEAMFS_DIRENT_ALIGN) - 1) / (BEAMFS_DIRENT_ALIGN)) *       \
	 (BEAMFS_DIRENT_ALIGN))

/*
 * The longest entry that still fits inside one subblock. Anything
 * longer is placed across a boundary rather than refused.
 */
#define BEAMFS_DIRENT_INLINE_MAX  BEAMFS_SUBBLOCK_DATA
#define BEAMFS_DIRENT_FITS_SUB(len)  ((len) <= BEAMFS_DIRENT_INLINE_MAX)

/* Returned by the placement helpers when a block cannot take an entry. */
#define BEAMFS_DIRENT_NOSPACE   ((u32)~0U)

/*
 * What the wall clock was worth when the anchor was taken.
 *
 * Not a measurement of the clock, a statement about it: the kernel
 * knows whether NTP considers itself synchronised, and nothing more.
 * A reader converting an entry to a date should carry this alongside
 * the date rather than dropping it.
 */
enum beamfs_clock_quality {
	/* No basis for the wall clock: no RTC, no sync. Dates from this
	 * anchor are ordering information wearing a timestamp's clothes.
	 */
	BEAMFS_CLOCK_UNKNOWN = 0,
	/* An RTC was read but nothing disciplines it. Seconds, maybe. */
	BEAMFS_CLOCK_RTC = 1,
	/* NTP reports itself synchronised. Milliseconds, typically. */
	BEAMFS_CLOCK_NTP = 2,
	/* A hardware-timestamped source: PTP, GPS. Microseconds or
	 * better, and the only case where fine dating is defensible.
	 */
	BEAMFS_CLOCK_HARDWARE = 3,
};

/* Symbols correctable per subblock; the budget saturates here. */
#define BEAMFS_ERROR_BUDGET_MAX   (BEAMFS_RS_PARITY / 2)

/*
 * One RS(255,239) codeword's parity, 16 bytes. Not the per-block cost,
 * which is BEAMFS_IND_PARITY_RS_BYTES or BEAMFS_IND_PARITY_CRC_BYTES.
 */
#define BEAMFS_IND_PARITY_BYTES   BEAMFS_RS_PARITY
/*
 * An indirect block is 4096 bytes, past the 255-byte RS codeword, so it
 * is split like a data block: 16 subblocks of 239 data bytes, with one
 * 16-byte parity set per subblock. That is 256 parity bytes per block,
 * over bytes [0, 3824): pointers 478..511 lie past the last subblock
 * and neither mode covers them.
 */
#define BEAMFS_IND_PARITY_PER_BLOCK  (BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_RS_PARITY)
/*
 * Slots in a raw 4096-byte region block, the geometry before
 * IND_PARITY_FEC. The kernel sizes slots with BEAMFS_IND_PARITY_RS_SLOTS
 * and BEAMFS_IND_PARITY_CRC_SLOTS.
 */
#define BEAMFS_IND_PARITY_PER_REGION_BLOCK  (BEAMFS_BLOCK_SIZE / BEAMFS_IND_PARITY_PER_BLOCK)

/* Flags supported by this kernel module.
 * PER_INODE_RS: per-inode RS(255,239) parity protection. The kernel
 * write path has always computed the parity on every inode write
 * (namei.c::beamfs_write_inode_raw); enabling this bit in SUPP
 * unlocks the read-side decoder under any s_data_protection_scheme.
 */
#define BEAMFS_FEAT_COMPAT_SUPP    0ULL
#define BEAMFS_FEAT_RO_COMPAT_SUPP (BEAMFS_FEATURE_RO_COMPAT_DATA_CSUM | \
				    BEAMFS_FEATURE_RO_COMPAT_DATA_SELFID)
#define BEAMFS_FEAT_INCOMPAT_SUPP  (BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS | \
				    BEAMFS_FEATURE_INCOMPAT_INDIRECT_PARITY | \
				    BEAMFS_FEATURE_INCOMPAT_ERROR_BUDGET | \
				    BEAMFS_FEATURE_INCOMPAT_DIR_RS | \
				    BEAMFS_FEATURE_INCOMPAT_INODE_CRC_FULL | \
				    BEAMFS_FEATURE_INCOMPAT_IND_PARITY_FEC | \
				    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE)

/*
 * On-disk superblock - block 0
 * Total size: fits in one 4096-byte block
 */
struct beamfs_super_block;

/*
 * beamfs_bitmap_blocks_count_from_flags -- number of on-disk bitmap blocks.
 *
 * Decoded from s_flags bits 0..15. 0 in the field means the volume was
 * formatted before multi-block bitmap support; treat as legacy single
 * bitmap block.
 */
static inline u32 beamfs_bitmap_blocks_count_from_flags(__le32 s_flags_le)
{
	u32 v = le32_to_cpu(s_flags_le) & BEAMFS_SB_FLAGS_BITMAP_BLOCKS_MASK;

	return v ? v : 1;
}

struct beamfs_super_block {
	__le32  s_magic;            /* BEAMFS_MAGIC */
	__le32  s_block_size;       /* Block size in bytes */
	__le64  s_block_count;      /* Total blocks */
	__le64  s_free_blocks;      /* Free blocks */
	__le64  s_inode_count;      /* Total inodes */
	__le64  s_free_inodes;      /* Free inodes */
	__le64  s_inode_table_blk;  /* Block where inode table starts */
	__le64  s_data_start_blk;   /* First data block */
	__le32  s_version;          /* Filesystem version */
	__le32  s_flags;            /* Flags */
	__le32  s_crc32;            /* CRC32 of this superblock */
	__u8    s_uuid[16];         /* UUID */
	__u8    s_label[32];        /* Volume label */
	struct beamfs_rs_event s_rs_journal[BEAMFS_RS_JOURNAL_SIZE]; /* 2560 bytes */
	__u8    s_rs_journal_head;  /* next write index (ring buffer) */
	__le64  s_bitmap_blk;       /* On-disk block bitmap block number */
	__le64  s_feat_compat;      /* Compatible feature flags (informational) */
	__le64  s_feat_incompat;    /* Incompatible features: refuse mount if unknown bit set */
	__le64  s_feat_ro_compat;   /* RO-compat features: force RO mount if unknown bit set */
	__le32  s_data_protection_scheme; /* enum BEAMFS_DATA_PROTECTION_* */
	/*
	 * Indirection parity region, valid when INCOMPAT_INDIRECT_PARITY
	 * is set. Zero on volumes without the feature.
	 *
	 * Placed before the superblock's own parity zone, as the comment
	 * on the superblock RS layout requires: new fields are taken out
	 * of s_pad ahead of it.
	 */
	__le64  s_ind_parity_blk;   /* first block of the parity region */
	__le32  s_ind_parity_len;   /* length of the region, in blocks */
	__le32  s_ind_parity_mode;  /* enum beamfs_ind_parity_mode */
	/*
	 * Error budget region, valid when INCOMPAT_ERROR_BUDGET is set.
	 * Zero on volumes formatted without it.
	 */
	__le64  s_budget_blk;       /* first block of the budget region */
	__le32  s_budget_len;       /* length of the region, in blocks */
	__le32  s_budget_pad;       /* zero */

	/*
	 * Clock anchor: what the two clocks read at the same instant.
	 *
	 * re_timestamp in a journal entry is ktime_get_ns() -- monotonic
	 * since boot, which orders events exactly and dates none of them.
	 * That is the right clock for the entry: it cannot jump, so the
	 * interval between two corrections is trustworthy to the timer's
	 * resolution, and a burst of forty upsets in three milliseconds
	 * reads as exactly that.
	 *
	 * What it cannot say is when. The anchor supplies the missing
	 * half: at the instant s_anchor_mono was sampled, the wall clock
	 * read s_anchor_real. Any entry converts to a date by adding the
	 * difference, and the conversion is only as good as the wall
	 * clock was -- which is why s_anchor_quality records what that
	 * clock was worth.
	 *
	 * Declaring the uncertainty rather than implying a precision is
	 * the point. A journal that claims nanosecond dating on an NTP
	 * machine is claiming something it cannot support, and an auditor
	 * who notices has reason to doubt the rest of it. Instrumentation
	 * practice is to state the source and its error; this follows it.
	 *
	 * Refreshed periodically by the scrubber, so drift is bounded by
	 * the refresh interval rather than accumulating from mount. A
	 * volume mounted for six months is otherwise dated by a clock
	 * reading six months stale.
	 */
	__le64  s_anchor_mono;      /* ktime_get_ns() at the anchor */
	__le64  s_anchor_real;      /* ktime_get_real_ns() at the same instant */
	__le32  s_anchor_quality;   /* enum beamfs_clock_quality */
	__le32  s_anchor_pad;       /* zero */
	__u8    s_pad[1327];        /* Padding; RS parity in its last 208 bytes */
} __packed;

/*
 * On-disk inode
 * Size: 256 bytes
 *
 * Addressing capacity:
 *   direct  (12)  =              48 KiB
 *   indirect (1)  =               2 MiB
 *   dindirect (1) =               1 GiB
 *   tindirect (1) =             512 GiB
 *
 * uid/gid: __le32 to support uid > 65535 (standard kernel convention)
 * timestamps: __le64 nanoseconds (required for space mission precision)
 */
struct beamfs_inode {
	__le16  i_mode;             /* File mode */
	__le16  i_nlink;            /* Hard link count */
	__le32  i_uid;              /* Owner UID */
	__le32  i_gid;              /* Owner GID */
	__le64  i_size;             /* File size in bytes (64-bit, future-proof) */
	__le64  i_atime;            /* Access time (ns) */
	__le64  i_mtime;            /* Modification time (ns) */
	__le64  i_ctime;            /* Change time (ns) */
	__le32  i_flags;            /* Inode flags */
	__le32  i_crc32;            /* CRC32 over [0, 48) and [52, 172) */
	__le64  i_direct[BEAMFS_DIRECT_BLOCKS];    /* Direct block pointers */
	__le64  i_indirect;         /* Single indirect (~2 MiB) */
	__le64  i_dindirect;        /* Double indirect (~1 GiB) */
	__le64  i_tindirect;        /* Triple indirect (~512 GiB) */
	__u8    i_reserved[84];     /* RS parity in [0, 16), then padding */
} __packed;

/*
 * Inode flags.
 *
 * BEAMFS_INODE_FL_RS_ENABLED: deprecated as of stage 3 (v0.3.0+).
 *   Was the per-inode opt-in for RS FEC under the
 *   BEAMFS_DATA_PROTECTION_INODE_OPT_IN scheme. Stage 3 replaces
 *   that scheme with BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL, where
 *   all inodes are RS-protected unconditionally. The flag is
 *   preserved in the bit definition so v0.1.0/v0.2.0 images that
 *   set it remain mountable; new images do not set it.
 *   See Documentation/threat-model.md section 6.1 and 6.3.
 */
#define BEAMFS_INODE_FL_RS_ENABLED   0x0001  /* deprecated, see comment */
#define BEAMFS_INODE_FL_VERIFIED     0x0002  /* Integrity verified */

/*
 * On-disk directory entry
 */
struct beamfs_dir_entry {
	__le64  d_ino;              /* Inode number */
	__le16  d_rec_len;          /* Record length */
	__u8    d_name_len;         /* Name length */
	__u8    d_file_type;        /* File type */
	char    d_name[BEAMFS_MAX_FILENAME + 1]; /* Filename */
} __packed;

#endif /* _BEAMFS_FORMAT_H */
