/*
 *  linux/fs/hfs/hfs_fs.h
 *
 * Copyright (C) 1995-1997  Paul H. Hargrove
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 * This file may be distributed under the terms of the GNU General Public License.
 */

#ifndef _LINUX_HFS_FS_H
#define _LINUX_HFS_FS_H

#ifdef pr_fmt
#undef pr_fmt
#endif

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/slab.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/workqueue.h>

#include <asm/byteorder.h>
#include <linux/uaccess.h>

#include "hfs.h"

#define DBG_BNODE_REFS	0x00000001
#define DBG_BNODE_MOD	0x00000002
#define DBG_CAT_MOD	0x00000004
#define DBG_INODE	0x00000008
#define DBG_SUPER	0x00000010
#define DBG_EXTENT	0x00000020
#define DBG_BITMAP	0x00000040

//#define DBG_MASK	(DBG_EXTENT|DBG_INODE|DBG_BNODE_MOD|DBG_CAT_MOD|DBG_BITMAP)
//#define DBG_MASK	(DBG_BNODE_MOD|DBG_CAT_MOD|DBG_INODE)
//#define DBG_MASK	(DBG_CAT_MOD|DBG_BNODE_REFS|DBG_INODE|DBG_EXTENT)
#define DBG_MASK	(0)

#define hfs_dbg(flg, fmt, ...)					\
do {								\
	if (DBG_##flg & DBG_MASK)				\
		printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__);	\
} while (0)

#define hfs_dbg_cont(flg, fmt, ...)				\
do {								\
	if (DBG_##flg & DBG_MASK)				\
		pr_cont(fmt, ##__VA_ARGS__);			\
} while (0)


/*
 * struct hfs_inode_info - HFS-specific inode information
 *
 * This structure contains HFS-specific metadata for each inode,
 * extending the standard Linux VFS inode with HFS filesystem details.
 * It handles file extents, resource forks, and catalog information.
 *
 * @opencnt: Number of open file handles
 * @flags: HFS-specific inode flags (HFS_FLG_*)
 * @tz_secondswest: Timezone offset in seconds west of GMT (to deal with localtime ugliness)
 * @cat_key: Catalog B-tree key for this inode
 * @open_dir_list: List of open directory entries
 * @open_dir_lock: Lock protecting open_dir_list
 * @rsrc_inode: Resource fork inode (if any)
 * @extents_lock: Mutex protecting extent information
 * @alloc_blocks: Number of allocated blocks for file
 * @clump_blocks: Clump size in allocation blocks
 * @fs_blocks: File size in filesystem blocks
 * @first_extents: First 3 extent records from catalog
 * @first_blocks: Number of blocks in first_extents
 * @cached_extents: Cached extent records from extents B-tree
 * @cached_start: Starting allocation block of cached extents
 * @cached_blocks: Number of blocks in cached_extents
 * @phys_size: Physical size of file on disk
 * @vfs_inode: Embedded VFS inode structure
 */
struct hfs_inode_info {
	atomic_t opencnt;

	unsigned int flags;

	int tz_secondswest;

	struct hfs_cat_key cat_key;

	struct list_head open_dir_list;
	spinlock_t open_dir_lock;
	struct inode *rsrc_inode;

	struct mutex extents_lock;

	u16 alloc_blocks, clump_blocks;
	sector_t fs_blocks;
	hfs_extent_rec first_extents;
	u16 first_blocks;
	hfs_extent_rec cached_extents;
	u16 cached_start, cached_blocks;

	loff_t phys_size;
	struct inode vfs_inode;
};

#define HFS_FLG_RSRC		0x0001
#define HFS_FLG_EXT_DIRTY	0x0002
#define HFS_FLG_EXT_NEW		0x0004

#define HFS_IS_RSRC(inode)	(HFS_I(inode)->flags & HFS_FLG_RSRC)

/*
 * struct hfs_sb_info - HFS-specific superblock information
 *
 * This structure contains all HFS-specific filesystem metadata,
 * extending the Linux VFS superblock. It manages the Master Directory
 * Block (MDB), allocation bitmap, B-trees, and various filesystem
 * parameters and mount options.
 *
 * @mdb_bh: Buffer holding the Master Directory Block (superblock/VIB/MDB)
 * @mdb: Pointer to parsed MDB structure
 * @alt_mdb_bh: Buffer holding the alternate copy of MDB
 * @alt_mdb: Pointer to alternate MDB
 * @bitmap: Allocation bitmap for tracking free/used blocks
 * @ext_tree: Extents overflow B-tree for managing file extents
 * @cat_tree: Catalog B-tree containing directory structure and metadata
 * @file_count: Total number of regular files in the filesystem
 * @folder_count: Total number of directories in the filesystem
 * @next_id: Next available catalog node ID for new files/directories
 * @clumpablks: Default clump size in allocation blocks for extending files
 * @fs_start: First 512-byte block represented in the allocation bitmap
 * @part_start: Starting block of HFS partition
 * @root_files: Number of regular files in the root directory
 * @root_dirs: Number of subdirectories in the root directory
 * @fs_ablocks: Total allocation blocks in the filesystem
 * @free_ablocks: Number of free allocation blocks available for allocation
 * @alloc_blksz: Size in bytes of each allocation block
 * @s_quiet: Suppress error messages for permission changes
 * @s_type: Default file type for new files
 * @s_creator: Default creator for new files
 * @s_file_umask: Permission mask applied to all regular files
 * @s_dir_umask: Permission mask applied to all directories
 * @s_uid: Default user ID for all files
 * @s_gid: Default group ID for all files
 * @session: CD-ROM session info
 * @part: CD-ROM partition info
 * @nls_io: Character encoding table for I/O
 * @nls_disk: Character encoding table for disk storage
 * @bitmap_lock: Mutex protecting bitmap operations
 * @flags: Filesystem state flags (HFS_FLG_*)
 * @blockoffset: Block offset within device
 * @fs_div: Filesystem block size divisor
 * @sb: Back-pointer to VFS superblock
 * @work_queued: Flag indicating delayed work is queued
 * @mdb_work: Delayed work for MDB writeback
 * @work_lock: Lock protecting work queue state
 */
struct hfs_sb_info {
	struct buffer_head *mdb_bh;
	struct hfs_mdb *mdb;
	struct buffer_head *alt_mdb_bh;
	struct hfs_mdb *alt_mdb;
	__be32 *bitmap;
	struct hfs_btree *ext_tree;
	struct hfs_btree *cat_tree;
	u32 file_count;
	u32 folder_count;
	u32 next_id;
	u32 clumpablks;
	u32 fs_start;
	u32 part_start;
	u16 root_files;
	u16 root_dirs;
	u16 fs_ablocks;
	u16 free_ablocks;
	u32 alloc_blksz;
	int s_quiet;
	__be32 s_type;
	__be32 s_creator;
	umode_t s_file_umask;
	umode_t s_dir_umask;
	kuid_t s_uid;
	kgid_t s_gid;

	int session, part;
	struct nls_table *nls_io, *nls_disk;
	struct mutex bitmap_lock;
	unsigned long flags;
	u16 blockoffset;
	int fs_div;
	struct super_block *sb;
	int work_queued;
	struct delayed_work mdb_work;
	spinlock_t work_lock;
};

#define HFS_FLG_BITMAP_DIRTY	0
#define HFS_FLG_MDB_DIRTY	1
#define HFS_FLG_ALT_MDB_DIRTY	2

/* bitmap.c */
extern u32 hfs_vbm_search_free(struct super_block *, u32, u32 *);
extern int hfs_clear_vbm_bits(struct super_block *, u16, u16);

/* catalog.c */
extern int hfs_cat_keycmp(const btree_key *, const btree_key *);
struct hfs_find_data;
extern int hfs_cat_find_brec(struct super_block *, u32, struct hfs_find_data *);
extern int hfs_cat_create(u32, struct inode *, const struct qstr *, struct inode *);
extern int hfs_cat_delete(u32, struct inode *, const struct qstr *);
extern int hfs_cat_move(u32, struct inode *, const struct qstr *,
			struct inode *, const struct qstr *);
extern void hfs_cat_build_key(struct super_block *, btree_key *, u32, const struct qstr *);

/* dir.c */
extern const struct file_operations hfs_dir_operations;
extern const struct inode_operations hfs_dir_inode_operations;

/* extent.c */
extern int hfs_ext_keycmp(const btree_key *, const btree_key *);
extern u16 hfs_ext_find_block(struct hfs_extent *ext, u16 off);
extern int hfs_free_fork(struct super_block *, struct hfs_cat_file *, int);
extern int hfs_ext_write_extent(struct inode *);
extern int hfs_extend_file(struct inode *);
extern void hfs_file_truncate(struct inode *);

extern int hfs_get_block(struct inode *, sector_t, struct buffer_head *, int);

/* inode.c */
extern const struct address_space_operations hfs_aops;
extern const struct address_space_operations hfs_btree_aops;

int hfs_write_begin(const struct kiocb *iocb, struct address_space *mapping,
		loff_t pos, unsigned len, struct folio **foliop, void **fsdata);
extern struct inode *hfs_new_inode(struct inode *, const struct qstr *, umode_t);
extern void hfs_inode_write_fork(struct inode *, struct hfs_extent *, __be32 *, __be32 *);
extern int hfs_write_inode(struct inode *, struct writeback_control *);
extern int hfs_inode_setattr(struct mnt_idmap *, struct dentry *,
			     struct iattr *);
extern void hfs_inode_read_fork(struct inode *inode, struct hfs_extent *ext,
			__be32 log_size, __be32 phys_size, u32 clump_size);
extern struct inode *hfs_iget(struct super_block *, struct hfs_cat_key *, hfs_cat_rec *);
extern void hfs_evict_inode(struct inode *);
extern void hfs_delete_inode(struct inode *);

/* attr.c */
extern const struct xattr_handler * const hfs_xattr_handlers[];

/* mdb.c */
extern int hfs_mdb_get(struct super_block *);
extern void hfs_mdb_commit(struct super_block *);
extern void hfs_mdb_close(struct super_block *);
extern void hfs_mdb_put(struct super_block *);

/* part_tbl.c */
extern int hfs_part_find(struct super_block *, sector_t *, sector_t *);

/* string.c */
extern const struct dentry_operations hfs_dentry_operations;

extern int hfs_hash_dentry(const struct dentry *, struct qstr *);
extern int hfs_strcmp(const unsigned char *, unsigned int,
		      const unsigned char *, unsigned int);
extern int hfs_compare_dentry(const struct dentry *dentry,
		unsigned int len, const char *str, const struct qstr *name);

/* trans.c */
extern void hfs_asc2mac(struct super_block *, struct hfs_name *, const struct qstr *);
extern int hfs_mac2asc(struct super_block *, char *, const struct hfs_name *);

/* super.c */
extern void hfs_mark_mdb_dirty(struct super_block *sb);

/*
 * There are two time systems.  Both are based on seconds since
 * a particular time/date.
 *	Unix:	signed little-endian since 00:00 GMT, Jan. 1, 1970
 *	mac:	unsigned big-endian since 00:00 GMT, Jan. 1, 1904
 *
 * HFS implementations are highly inconsistent, this one matches the
 * traditional behavior of 64-bit Linux, giving the most useful
 * time range between 1970 and 2106, by treating any on-disk timestamp
 * under HFS_UTC_OFFSET (Jan 1 1970) as a time between 2040 and 2106.
 */
#define HFS_UTC_OFFSET 2082844800U

static inline time64_t __hfs_m_to_utime(__be32 mt)
{
	time64_t ut = (u32)(be32_to_cpu(mt) - HFS_UTC_OFFSET);

	return ut + sys_tz.tz_minuteswest * 60;
}

static inline __be32 __hfs_u_to_mtime(time64_t ut)
{
	ut -= sys_tz.tz_minuteswest * 60;

	return cpu_to_be32(lower_32_bits(ut) + HFS_UTC_OFFSET);
}
#define HFS_I(inode)	(container_of(inode, struct hfs_inode_info, vfs_inode))
#define HFS_SB(sb)	((struct hfs_sb_info *)(sb)->s_fs_info)

#define hfs_m_to_utime(time)   (struct timespec64){ .tv_sec = __hfs_m_to_utime(time) }
#define hfs_u_to_mtime(time)   __hfs_u_to_mtime((time).tv_sec)
#define hfs_mtime()		__hfs_u_to_mtime(ktime_get_real_seconds())

static inline const char *hfs_mdb_name(struct super_block *sb)
{
	return sb->s_id;
}

static inline void hfs_bitmap_dirty(struct super_block *sb)
{
	set_bit(HFS_FLG_BITMAP_DIRTY, &HFS_SB(sb)->flags);
	hfs_mark_mdb_dirty(sb);
}

#define sb_bread512(sb, sec, data) ({			\
	struct buffer_head *__bh;			\
	sector_t __block;				\
	loff_t __start;					\
	int __offset;					\
							\
	__start = (loff_t)(sec) << HFS_SECTOR_SIZE_BITS;\
	__block = __start >> (sb)->s_blocksize_bits;	\
	__offset = __start & ((sb)->s_blocksize - 1);	\
	__bh = sb_bread((sb), __block);			\
	if (likely(__bh != NULL))			\
		data = (void *)(__bh->b_data + __offset);\
	else						\
		data = NULL;				\
	__bh;						\
})

#endif
