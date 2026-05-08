// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext4/bitmap.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 */

#include <linux/buffer_head.h>
#include "ext4.h"

unsigned int ext4_count_free(char *bitmap, unsigned int numchars)
{
	return numchars * BITS_PER_BYTE - memweight(bitmap, numchars);
}

static inline __u32 ext4_inode_bitmap_csum_get(struct super_block *sb,
					       struct ext4_group_desc *gdp)
{
	__u32 csum = le16_to_cpu(gdp->bg_inode_bitmap_csum_lo);

	if (EXT4_DESC_SIZE(sb) >= EXT4_BG_INODE_BITMAP_CSUM_HI_END)
		csum |= (__u32)le16_to_cpu(gdp->bg_inode_bitmap_csum_hi) << 16;
	return csum;
}

static inline void ext4_inode_bitmap_csum_store(struct super_block *sb,
						struct ext4_group_desc *gdp,
						__u32 csum)
{
	gdp->bg_inode_bitmap_csum_lo = cpu_to_le16(csum & 0xFFFF);
	if (EXT4_DESC_SIZE(sb) >= EXT4_BG_INODE_BITMAP_CSUM_HI_END)
		gdp->bg_inode_bitmap_csum_hi = cpu_to_le16(csum >> 16);
}

int ext4_inode_bitmap_csum_verify(struct super_block *sb,
				  struct ext4_group_desc *gdp,
				  struct buffer_head *bh)
{
	__u32 provided, calculated;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int sz;

	if (!ext4_has_feature_metadata_csum(sb))
		return 1;

	sz = EXT4_INODES_PER_GROUP(sb) >> 3;
	provided = ext4_inode_bitmap_csum_get(sb, gdp);
	calculated = ext4_chksum(sbi->s_csum_seed, (__u8 *)bh->b_data, sz);
	if (EXT4_DESC_SIZE(sb) < EXT4_BG_INODE_BITMAP_CSUM_HI_END)
		calculated &= 0xFFFF;

	return provided == calculated;
}

void ext4_inode_bitmap_csum_set(struct super_block *sb,
				struct ext4_group_desc *gdp,
				struct buffer_head *bh)
{
	__u32 csum;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int sz;

	if (!ext4_has_feature_metadata_csum(sb))
		return;

	sz = EXT4_INODES_PER_GROUP(sb) >> 3;
	csum = ext4_chksum(sbi->s_csum_seed, (__u8 *)bh->b_data, sz);
	ext4_inode_bitmap_csum_store(sb, gdp, csum);
}

static inline __u32 ext4_block_bitmap_csum_get(struct super_block *sb,
					       struct ext4_group_desc *gdp)
{
	__u32 csum = le16_to_cpu(gdp->bg_block_bitmap_csum_lo);

	if (EXT4_DESC_SIZE(sb) >= EXT4_BG_BLOCK_BITMAP_CSUM_HI_END)
		csum |= (__u32)le16_to_cpu(gdp->bg_block_bitmap_csum_hi) << 16;
	return csum;
}

static inline void ext4_block_bitmap_csum_store(struct super_block *sb,
						struct ext4_group_desc *gdp,
						__u32 csum)
{
	gdp->bg_block_bitmap_csum_lo = cpu_to_le16(csum & 0xFFFF);
	if (EXT4_DESC_SIZE(sb) >= EXT4_BG_BLOCK_BITMAP_CSUM_HI_END)
		gdp->bg_block_bitmap_csum_hi = cpu_to_le16(csum >> 16);
}

int ext4_block_bitmap_csum_verify(struct super_block *sb,
				  struct ext4_group_desc *gdp,
				  struct buffer_head *bh)
{
	__u32 provided, calculated;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int sz = EXT4_CLUSTERS_PER_GROUP(sb) / 8;

	if (!ext4_has_feature_metadata_csum(sb))
		return 1;

	provided = ext4_block_bitmap_csum_get(sb, gdp);
	calculated = ext4_chksum(sbi->s_csum_seed, (__u8 *)bh->b_data, sz);
	if (EXT4_DESC_SIZE(sb) < EXT4_BG_BLOCK_BITMAP_CSUM_HI_END)
		calculated &= 0xFFFF;

	return provided == calculated;
}

void ext4_block_bitmap_csum_set(struct super_block *sb,
				struct ext4_group_desc *gdp,
				struct buffer_head *bh)
{
	int sz = EXT4_CLUSTERS_PER_GROUP(sb) / 8;
	__u32 csum;
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	if (!ext4_has_feature_metadata_csum(sb))
		return;

	csum = ext4_chksum(sbi->s_csum_seed, (__u8 *)bh->b_data, sz);
	ext4_block_bitmap_csum_store(sb, gdp, csum);
}

/*
 * Update block bitmap checksum using incremental CRC calculation.
 *
 * This function assumes that ALL bits in the range [offset, offset+len)
 * have been flipped (XORed with 1). It uses crc32c_flip_range() to
 * efficiently compute the CRC delta without re-scanning the entire bitmap.
 * The csum_seed cancels out in the XOR delta, so it is not needed here.
 */
void ext4_block_bitmap_csum_set_range(struct super_block *sb,
				      struct ext4_group_desc *gdp,
				      ext4_grpblk_t offset, ext4_grpblk_t len)
{
	__u32 new_csum, old_csum;

	if (!ext4_has_feature_metadata_csum(sb))
		return;

	old_csum = ext4_block_bitmap_csum_get(sb, gdp);
	new_csum = crc32c_flip_range(old_csum, EXT4_CLUSTERS_PER_GROUP(sb),
				     offset, len);

	ext4_block_bitmap_csum_store(sb, gdp, new_csum);
}
