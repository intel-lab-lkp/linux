/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FTRFS — Fault-Tolerant Radiation-Robust Filesystem
 * Based on: Fuchs, Langer, Trinitis — ARCS 2015
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#ifndef _FTRFS_H
#define _FTRFS_H

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/types.h>

/* inode_state_read_once returns inode_state_flags in kernel 7.0 */
#define ftrfs_inode_is_new(inode) \
	(inode_state_read_once(inode) & I_NEW)

/* Magic number: 'FTRF' */
#define FTRFS_MAGIC         0x46545246

/* Block size: 4096 bytes */
#define FTRFS_BLOCK_SIZE    4096
#define FTRFS_BLOCK_SHIFT   12

/* RS FEC: 16 parity bytes per 239-byte subblock (RS(255,239)) */
#define FTRFS_RS_PARITY     16
#define FTRFS_SUBBLOCK_DATA 239
#define FTRFS_SUBBLOCK_TOTAL (FTRFS_SUBBLOCK_DATA + FTRFS_RS_PARITY)

/* Filesystem limits */
#define FTRFS_MAX_FILENAME  255
#define FTRFS_DIRECT_BLOCKS 12
#define FTRFS_INDIRECT_BLOCKS 1
#define FTRFS_DINDIRECT_BLOCKS 1

/*
 * Radiation Event Journal entry — 24 bytes
 * Records each RS FEC correction event persistently in the superblock.
 * 64 entries give operators a map of physical degradation over time.
 * No existing Linux filesystem provides this at the block layer.
 */
struct ftrfs_rs_event {
	__le64  re_block_no;    /* corrected block number */
	__le64  re_timestamp;   /* nanoseconds since boot  */
	__le32  re_error_bits;  /* number of symbols corrected */
	__le32  re_crc32;       /* CRC32 of this entry */
} __packed;                 /* 24 bytes */

#define FTRFS_RS_JOURNAL_SIZE  64   /* entries in the radiation event journal */

/*
 * On-disk superblock — block 0
 * Total size: fits in one 4096-byte block
 */
struct ftrfs_super_block {
	__le32  s_magic;            /* FTRFS_MAGIC */
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
	 struct ftrfs_rs_event s_rs_journal[FTRFS_RS_JOURNAL_SIZE]; /* 1536 bytes */
	__u8    s_rs_journal_head;  /* next write index (ring buffer) */
	__u8    s_pad[2443];        /* Padding to 4096 bytes */
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
struct ftrfs_inode {
	__le16  i_mode;             /* File mode */
	__le16  i_nlink;            /* Hard link count */
	__le32  i_uid;              /* Owner UID */
	__le32  i_gid;              /* Owner GID */
	__le64  i_size;             /* File size in bytes (64-bit, future-proof) */
	__le64  i_atime;            /* Access time (ns) */
	__le64  i_mtime;            /* Modification time (ns) */
	__le64  i_ctime;            /* Change time (ns) */
	__le32  i_flags;            /* Inode flags */
	__le32  i_crc32;            /* CRC32 of inode (excluding this field) */
	__le64  i_direct[FTRFS_DIRECT_BLOCKS];    /* Direct block pointers */
	__le64  i_indirect;         /* Single indirect (~2 MiB) */
	__le64  i_dindirect;        /* Double indirect (~1 GiB) */
	__le64  i_tindirect;        /* Triple indirect (~512 GiB) */
	__u8    i_reserved[84];     /* Padding to 256 bytes */
} __packed;

/* Inode flags */
#define FTRFS_INODE_FL_RS_ENABLED   0x0001  /* RS FEC enabled */
#define FTRFS_INODE_FL_VERIFIED     0x0002  /* Integrity verified */

/*
 * On-disk directory entry
 */
struct ftrfs_dir_entry {
	__le64  d_ino;              /* Inode number */
	__le16  d_rec_len;          /* Record length */
	__u8    d_name_len;         /* Name length */
	__u8    d_file_type;        /* File type */
	char    d_name[FTRFS_MAX_FILENAME + 1]; /* Filename */
} __packed;

/*
 * In-memory superblock info (stored in sb->s_fs_info)
 */
struct ftrfs_sb_info {
	/* Block allocator */
	unsigned long    *s_block_bitmap;  /* In-memory free block bitmap */
	unsigned long     s_nblocks;       /* Number of data blocks */
	unsigned long     s_data_start;    /* First data block number */
	struct ftrfs_super_block *s_ftrfs_sb; /* On-disk superblock copy */
	struct buffer_head       *s_sbh;      /* Buffer head for superblock */
	spinlock_t                s_lock;     /* Superblock lock */
	unsigned long             s_free_blocks;
	unsigned long             s_free_inodes;
};

/*
 * In-memory inode info (embedded in VFS inode via container_of)
 */
struct ftrfs_inode_info {
	__le64          i_direct[FTRFS_DIRECT_BLOCKS];
	__le64          i_indirect;
	__le64          i_dindirect;
	__le64          i_tindirect;
	__u32           i_flags;
	struct inode    vfs_inode;  /* Must be last */
};

static inline struct ftrfs_inode_info *FTRFS_I(struct inode *inode)
{
	return container_of(inode, struct ftrfs_inode_info, vfs_inode);
}

static inline struct ftrfs_sb_info *FTRFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* Function prototypes */
/* super.c */
int ftrfs_fill_super(struct super_block *sb, struct fs_context *fc);
void ftrfs_log_rs_event(struct super_block *sb, u64 block_no, u32 err_bits);

/* inode.c */
struct inode *ftrfs_iget(struct super_block *sb, unsigned long ino);
struct inode *ftrfs_new_inode(struct inode *dir, umode_t mode);

/* dir.c */
extern const struct file_operations ftrfs_dir_operations;
extern const struct inode_operations ftrfs_dir_inode_operations;

/* file.c */
extern const struct file_operations ftrfs_file_operations;
extern const struct inode_operations ftrfs_file_inode_operations;
extern const struct address_space_operations ftrfs_aops;

/* edac.c */
__u32 ftrfs_crc32(const void *buf, size_t len);
int ftrfs_rs_encode(uint8_t *data, uint8_t *parity);
int ftrfs_rs_decode(uint8_t *data, uint8_t *parity);

/* block.c */

#endif /* _FTRFS_H */

/*
 */

/* alloc.c */
int  ftrfs_setup_bitmap(struct super_block *sb);
void ftrfs_destroy_bitmap(struct super_block *sb);
u64  ftrfs_alloc_block(struct super_block *sb);
void ftrfs_free_block(struct super_block *sb, u64 block);
u64  ftrfs_alloc_inode_num(struct super_block *sb);

/* dir.c */
struct dentry *ftrfs_lookup(struct inode *dir, struct dentry *dentry,
			    unsigned int flags);

/* namei.c */
int ftrfs_write_inode(struct inode *inode, struct writeback_control *wbc);
