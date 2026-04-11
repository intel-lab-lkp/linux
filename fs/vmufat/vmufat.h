// SPDX-License-Identifier: GPL-2.0+
/* Header file for VMUFAT filesystem */
#ifndef _VMUFAT_H_
#define _VMUFAT_H_

/* maximum length of file name */
#define VMUFAT_NAMELEN			12

/* Some GNU utils might not list files with inode num 0 */
#define VMUFAT_ZEROBLOCK		0x10000
#define VMU_BLK_SZ			0x200
#define	VMU_BLK_SZ16			0x100
#define VMU_PHYS_SZ			0x100

/* file allocation table markers */
#define VMUFAT_FILE_END			0xFFFA
#define VMUFAT_UNALLOCATED		0xFFFC
#define VMUFAT_ERROR			0xFFFF

/* parameters for possible VMU volume sizes */
#define VMUFAT_MIN_BLK			0x04
#define VMUFAT_MAX_BLK			0x10000

/* specifcations for directory entries */
#define VMU_DIR_RECORD_LEN		0x20
#define VMU_DIR_RECORD_LEN16		0x10
#define VMU_DIR_ENTRIES_PER_BLOCK	0x10
#define VMUFAT_NAME_OFFSET		0x04
#define VMUFAT_FIRSTBLOCK_OFFSET16	0x01
#define VMUFAT_START_OFFSET16		0x01
#define VMUFAT_SIZE_OFFSET16		0x0C
#define VMUFAT_HEADER_OFFSET16		0x0D

/* File types used in directory */
#define VMU_GAME			0xCC
#define VMU_DATA			0x33

/* filesystem locations marked in the root block */
#define VMU_LOCATION_FAT		0x23
#define VMU_LOCATION_FATLEN		0x24
#define VMU_LOCATION_DIR		0x25
#define VMU_LOCATION_DIRLEN		0x26
#define VMU_LOCATION_USRLEN		0x28 /* set to 200 by real hardware */

/* date offsets */
#define VMUFAT_SB_DATEOFFSET	0x30
#define VMUFAT_FILE_DATEOFFSET	0x10

#define EXEC				0111
#define NOCOPY				0xFF
#define CANCOPY				0x00

u16 vmufat_get_fat(struct super_block *sb, long block);
void vmufat_save_bcd(struct inode *in, char *bh, int index_to_dir);
struct inode *vmufat_get_inode(struct super_block *sb, long ino);
int vmufat_list_blocks(struct inode *in);

enum vmufat_date {
	VMUFAT_DIR_CENT	=		0x10,
	VMUFAT_DIR_YEAR,
	VMUFAT_DIR_MONTH,
	VMUFAT_DIR_DAY,
	VMUFAT_DIR_HOUR,
	VMUFAT_DIR_MIN,
	VMUFAT_DIR_SEC,
	VMUFAT_DIR_DOW
};

/* constants for BCD conversion */
#define SECONDS_PER_DAY			86400
#define DAYS_PER_YEAR			365
#define SECONDS_PER_HOUR		3600
#define HOURS_PER_DAY			24
#define SIXTY_MINS_OR_SECS		60
#define FEB28				59
#define DAYS_PER_WEEK			7
#define START_OF_EPOCH			70
#define CENTURY21			30
#define CENTURY22			130

struct memcard {
	unsigned int sb_bnum;
	unsigned int fat_bnum;
	unsigned int fat_len;
	unsigned int dir_bnum;
	unsigned int dir_len;
	unsigned int numblocks;
	struct mutex mutex;
};

struct vmufat_block {
	struct list_head b_list;
	int bno;
};

struct vmufat_inode {
	struct list_head blocks;
	unsigned int nblcks;
	enum vmufat_file_type {
		GAME = 1,
		DATA = 2
	} ft;
	struct inode vfs_inode;
};

static inline struct vmufat_inode *VMUFAT_I(struct inode *in)
{
	return container_of(in, struct vmufat_inode, vfs_inode);
}

struct vmufat_file_info {
	u8 ftype;
	int fblk;
	char fname[VMUFAT_NAMELEN];
};

static struct buffer_head *vmufat_sb_bread(struct super_block *sb,
	sector_t block)
{
	if (!sb)
		return NULL;
	return sb_bread(sb, block);
}

int vmufat_count_files(struct super_block *sb);
#endif
