/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  linux/include/linux/hfsplus_raw.h
 *
 * Copyright (C) 1999
 * Brad Boyer (flar@pants.nu)
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 *
 * Format of structures on disk
 * Information taken from Apple Technote #1150 (HFS Plus Volume Format)
 *
 */

#ifndef _LINUX_HFSPLUS_RAW_H
#define _LINUX_HFSPLUS_RAW_H

#include <linux/types.h>

/* Some constants */
#define HFSPLUS_SECTOR_SIZE        512
#define HFSPLUS_SECTOR_SHIFT         9
#define HFSPLUS_VOLHEAD_SECTOR       2
#define HFSPLUS_VOLHEAD_SIG     0x482b
#define HFSPLUS_VOLHEAD_SIGX    0x4858
#define HFSPLUS_SUPER_MAGIC     0x482b
#define HFSPLUS_MIN_VERSION          4
#define HFSPLUS_CURRENT_VERSION      5

#define HFSP_WRAP_MAGIC         0x4244
#define HFSP_WRAP_ATTRIB_SLOCK  0x8000
#define HFSP_WRAP_ATTRIB_SPARED 0x0200

#define HFSP_WRAPOFF_SIG          0x00
#define HFSP_WRAPOFF_ATTRIB       0x0A
#define HFSP_WRAPOFF_ABLKSIZE     0x14
#define HFSP_WRAPOFF_ABLKSTART    0x1C
#define HFSP_WRAPOFF_EMBEDSIG     0x7C
#define HFSP_WRAPOFF_EMBEDEXT     0x7E

#define HFSP_HIDDENDIR_NAME \
	"\xe2\x90\x80\xe2\x90\x80\xe2\x90\x80\xe2\x90\x80HFS+ Private Data"

#define HFSP_HARDLINK_TYPE	0x686c6e6b	/* 'hlnk' */
#define HFSP_HFSPLUS_CREATOR	0x6866732b	/* 'hfs+' */

#define HFSP_SYMLINK_TYPE	0x736c6e6b	/* 'slnk' */
#define HFSP_SYMLINK_CREATOR	0x72686170	/* 'rhap' */

#define HFSP_MOUNT_VERSION	0x482b4c78	/* 'H+Lx' */

/* Structures used on disk */

typedef __be32 hfsplus_cnid;
typedef __be16 hfsplus_unichr;

#define HFSPLUS_MAX_STRLEN 255
#define HFSPLUS_ATTR_MAX_STRLEN 127

/*
 * struct hfsplus_unistr - HFS+ Unicode string
 *
 * HFS+ uses UTF-16 Unicode strings for filenames and other text data.
 * The length field specifies the number of Unicode characters (not bytes).
 * Maximum filename length is 255 Unicode characters.
 *
 * @length: Number of Unicode characters
 * @unicode: UTF-16 character array
 */
struct hfsplus_unistr {
/* 0x000 */
	__be16 length;					/* 0x00 */
	hfsplus_unichr unicode[HFSPLUS_MAX_STRLEN];	/* 0x02 */
/* 0x200 */
} __packed;

/*
 * struct hfsplus_attr_unistr - HFS+ extended attribute name string
 *
 * Similar to hfsplus_unistr but with a shorter maximum length for
 * extended attribute names. Used in the attributes B-tree for
 * storing extended attribute identifiers.
 *
 * @length: Number of Unicode characters
 * @unicode: UTF-16 characters
 */
struct hfsplus_attr_unistr {
/* 0x000 */
	__be16 length;						/* 0x00 */
	hfsplus_unichr unicode[HFSPLUS_ATTR_MAX_STRLEN];	/* 0x02 */
/* 0x100 */
} __packed;

/*
 * struct hfsplus_perm - HFS+ POSIX permissions and BSD flags
 *
 * Stores POSIX-style permissions along with BSD user and system flags.
 * This structure provides Unix-style access control for HFS+ files
 * and directories.
 *
 * @owner: User ID of file owner
 * @group: Group ID of file
 * @rootflags: BSD system flags (root-only)
 * @userflags: BSD user flags (immutable, append, etc.)
 * @mode: POSIX permission mode bits
 * @dev: Device ID for special files
 */
struct hfsplus_perm {
/* 0x00 */
	__be32 owner;		/* 0x00 */
	__be32 group;		/* 0x04 */
	u8  rootflags;		/* 0x08 */
	u8  userflags;		/* 0x09 */
	__be16 mode;		/* 0x0A */
	__be32 dev;		/* 0x0C */
/* 0x10 */
} __packed;

#define HFSPLUS_FLG_NODUMP	0x01
#define HFSPLUS_FLG_IMMUTABLE	0x02
#define HFSPLUS_FLG_APPEND	0x04

/*
 * struct hfsplus_extent - File extent descriptor
 *
 * Describes a single contiguous range of allocation blocks belonging
 * to a file. HFS+ files can be fragmented across multiple extents
 * to efficiently use disk space.
 *
 * @start_block: Starting allocation block number
 * @block_count: Number of contiguous blocks
 */
struct hfsplus_extent {
/* 0x00 */
	__be32 start_block;		/* 0x00 */
	__be32 block_count;		/* 0x04 */
/* 0x08 */
} __packed;

/*
 * hfsplus_extent_rec - Array of 8 extent descriptors
 *
 * HFS+ stores up to 8 extent records directly in catalog entries,
 * compared to 3 in classic HFS. Additional extents for highly
 * fragmented files are stored in the extents overflow B-tree.
 */
typedef struct hfsplus_extent hfsplus_extent_rec[8];

/*
 * struct hfsplus_fork_raw - HFS+ fork information
 *
 * Contains complete information about a file fork (data or resource).
 * Each HFS+ file can have two forks: a data fork for file content
 * and a resource fork for metadata, icons, and application data.
 *
 * @total_size: Logical size of fork in bytes
 * @clump_size: Preferred allocation clump size
 * @total_blocks: Total allocation blocks used by fork
 * @extents: First 8 extent descriptors
 */
struct hfsplus_fork_raw {
/* 0x00 */
	__be64 total_size;		/* 0x00 */
	__be32 clump_size;		/* 0x08 */
	__be32 total_blocks;		/* 0x0C */
	hfsplus_extent_rec extents;	/* 0x10 */
/* 0x50 */
} __packed;

/*
 * struct hfsplus_vh - HFS+ Volume Header
 *
 * The Volume Header is the primary metadata structure of an HFS+ filesystem,
 * equivalent to a superblock. It contains essential filesystem parameters,
 * allocation information, timestamps, and fork descriptors for special files.
 * Located at sector 2 of the HFS+ partition, with a backup copy at the
 * second-to-last sector.
 *
 * @signature: Volume signature (0x482b for HFS+, 0x4858 for HFSX)
 * @version: Volume format version (4 or 5)
 * @attributes: Volume attribute flags
 * @last_mount_vers: Implementation version that last mounted volume
 * @reserved: Reserved field
 * @create_date: Volume creation timestamp
 * @modify_date: Volume modification timestamp
 * @backup_date: Last backup timestamp
 * @checked_date: Last filesystem check timestamp
 * @file_count: Total number of files in filesystem
 * @folder_count: Total number of folders in filesystem
 * @blocksize: Allocation block size in bytes
 * @total_blocks: Total number of allocation blocks
 * @free_blocks: Number of free allocation blocks
 * @next_alloc: Next allocation search start point
 * @rsrc_clump_sz: Default resource fork clump size
 * @data_clump_sz: Default data fork clump size
 * @next_cnid: Next available catalog node ID
 * @write_count: Volume write operation counter
 * @encodings_bmp: Bitmap of supported text encodings
 * @finder_info: Finder information and boot parameters
 * @alloc_file: Allocation bitmap file fork
 * @ext_file: Extents overflow B-tree file fork
 * @cat_file: Catalog B-tree file fork
 * @attr_file: Attributes B-tree file fork
 * @start_file: Startup file fork (boot support)
 */
struct hfsplus_vh {
/* 0x000 */
	__be16 signature;		/* 0x00 */
	__be16 version;			/* 0x02 */
	__be32 attributes;		/* 0x04 */
	__be32 last_mount_vers;		/* 0x08 */
	u32 reserved;			/* 0x0C */

	__be32 create_date;		/* 0x10 */
	__be32 modify_date;		/* 0x14 */
	__be32 backup_date;		/* 0x18 */
	__be32 checked_date;		/* 0x1C */

	__be32 file_count;		/* 0x20 */
	__be32 folder_count;		/* 0x24 */

	__be32 blocksize;		/* 0x28 */
	__be32 total_blocks;		/* 0x2C */
	__be32 free_blocks;		/* 0x30 */

	__be32 next_alloc;		/* 0x34 */
	__be32 rsrc_clump_sz;		/* 0x38 */
	__be32 data_clump_sz;		/* 0x3C */
	hfsplus_cnid next_cnid;		/* 0x40 */

	__be32 write_count;		/* 0x44 */
	__be64 encodings_bmp;		/* 0x48 */

	u32 finder_info[8];		/* 0x50 */

	struct hfsplus_fork_raw alloc_file;	/* 0x70 */
	struct hfsplus_fork_raw ext_file;	/* 0xC0 */
	struct hfsplus_fork_raw cat_file;	/* 0x110 */
	struct hfsplus_fork_raw attr_file;	/* 0x160 */
	struct hfsplus_fork_raw start_file;	/* 0x1B0 */
/* 0x200 */
} __packed;

/* HFS+ volume attributes */
#define HFSPLUS_VOL_UNMNT		(1 << 8)
#define HFSPLUS_VOL_SPARE_BLK		(1 << 9)
#define HFSPLUS_VOL_NOCACHE		(1 << 10)
#define HFSPLUS_VOL_INCNSTNT		(1 << 11)
#define HFSPLUS_VOL_NODEID_REUSED	(1 << 12)
#define HFSPLUS_VOL_JOURNALED		(1 << 13)
#define HFSPLUS_VOL_SOFTLOCK		(1 << 15)
#define HFSPLUS_VOL_UNUSED_NODE_FIX	(1 << 31)

/*
 * struct hfs_bnode_desc - HFS+ B-tree node descriptor
 *
 * This structure appears at the beginning of every HFS+ B-tree node
 * on disk, providing essential metadata for navigating the B-tree
 * structure and understanding the node's contents.
 *
 * @next: Node ID of next node at same level
 * @prev: Node ID of previous node at same level
 * @type: Node type (index/header/map/leaf)
 * @height: Distance from leaves (leaves=1)
 * @num_recs: Number of records stored in this node
 * @reserved: Reserved space for alignment
 */
struct hfs_bnode_desc {
/* 0x00 */
	__be32 next;			/* 0x00 */
	__be32 prev;			/* 0x04 */
	s8 type;			/* 0x08 */
	u8 height;			/* 0x09 */
	__be16 num_recs;		/* 0x0A */
	u16 reserved;			/* 0x0C */
/* 0x0E */
} __packed;

/* HFS+ BTree node types */
#define HFS_NODE_INDEX	0x00	/* An internal (index) node */
#define HFS_NODE_HEADER	0x01	/* The tree header node (node 0) */
#define HFS_NODE_MAP	0x02	/* Holds part of the bitmap of used nodes */
#define HFS_NODE_LEAF	0xFF	/* A leaf (ndNHeight==1) node */

/*
 * struct hfs_btree_header_rec - HFS+ B-tree header record
 *
 * This structure is stored as the first record in the header node
 * (node 0) of every HFS+ B-tree. It contains essential metadata about
 * the B-tree structure, organization, and current state.
 *
 * @depth: Number of levels in B-tree (root to leaf)
 * @root: Node ID of the root node
 * @leaf_count: Total number of data records in leaves
 * @leaf_head: Node number of first (leftmost) leaf
 * @leaf_tail: Node number of last (rightmost) leaf
 * @node_size: Size of each B-tree node in bytes
 * @max_key_len: Maximum key length for index nodes
 * @node_count: Total number of nodes allocated
 * @free_nodes: Number of unused nodes available
 * @reserved1: Reserved field for future use
 * @clump_size: Allocation clump size for B-tree growth
 * @btree_type: Type identifier for this B-tree
 * @key_type: Key comparison type (case-sensitive/insensitive)
 * @attributes: B-tree feature flags and attributes
 * @reserved3: Reserved space for future expansion
 */
struct hfs_btree_header_rec {
/* 0x00 */
	__be16 depth;			/* 0x00 */
	__be32 root;			/* 0x02 */
	__be32 leaf_count;		/* 0x06 */
	__be32 leaf_head;		/* 0x0A */
	__be32 leaf_tail;		/* 0x0E */
	__be16 node_size;		/* 0x12 */
	__be16 max_key_len;		/* 0x14 */
	__be32 node_count;		/* 0x16 */
	__be32 free_nodes;		/* 0x1A */
	u16 reserved1;			/* 0x1E */
	__be32 clump_size;		/* 0x20 */
	u8 btree_type;			/* 0x24 */
	u8 key_type;			/* 0x25 */
	__be32 attributes;		/* 0x26 */
	u32 reserved3[16];		/* 0x2A */
/* 0x6A */
} __packed;

/* BTree attributes */
#define HFS_TREE_BIGKEYS	2
#define HFS_TREE_VARIDXKEYS	4

/* HFS+ BTree misc info */
#define HFSPLUS_TREE_HEAD 0
#define HFSPLUS_NODE_MXSZ 32768
#define HFSPLUS_ATTR_TREE_NODE_SIZE		8192
#define HFSPLUS_BTREE_HDR_NODE_RECS_COUNT	3
#define HFSPLUS_BTREE_HDR_USER_BYTES		128

/* Some special File ID numbers (stolen from hfs.h) */
#define HFSPLUS_POR_CNID		1	/* Parent Of the Root */
#define HFSPLUS_ROOT_CNID		2	/* ROOT directory */
#define HFSPLUS_EXT_CNID		3	/* EXTents B-tree */
#define HFSPLUS_CAT_CNID		4	/* CATalog B-tree */
#define HFSPLUS_BAD_CNID		5	/* BAD blocks file */
#define HFSPLUS_ALLOC_CNID		6	/* ALLOCation file */
#define HFSPLUS_START_CNID		7	/* STARTup file */
#define HFSPLUS_ATTR_CNID		8	/* ATTRibutes file */
#define HFSPLUS_EXCH_CNID		15	/* ExchangeFiles temp id */
#define HFSPLUS_FIRSTUSER_CNID		16	/* first available user id */

/* btree key type */
#define HFSPLUS_KEY_CASEFOLDING		0xCF	/* case-insensitive */
#define HFSPLUS_KEY_BINARY		0xBC	/* case-sensitive */

/*
 * struct hfsplus_cat_key - HFS+ catalog B-tree search key
 *
 * Key structure used for searching the catalog B-tree. Entries are
 * organized by parent directory ID and filename, allowing efficient
 * lookups and directory traversals with Unicode filename support.
 *
 * @key_len: Total length of key in bytes
 * @parent: Catalog Node ID of parent directory
 * @name: Unicode filename within parent directory
 */
struct hfsplus_cat_key {
/* 0x000 */
	__be16 key_len;			/* 0x00 */
	hfsplus_cnid parent;		/* 0x02 */
	struct hfsplus_unistr name;	/* 0x06 */
/* 0x206 */
} __packed;

#define HFSPLUS_CAT_KEYLEN	(sizeof(struct hfsplus_cat_key))

/*
 * struct hfsp_point - 2D coordinate point for Finder display
 *
 * Used by the Finder for positioning icons and windows in the
 * Mac OS graphical interface.
 *
 * @v: Vertical coordinate
 * @h: Horizontal coordinate
 */
struct hfsp_point {
/* 0x00 */
	__be16 v;			/* 0x00 */
	__be16 h;			/* 0x02 */
/* 0x04 */
} __packed;

/*
 * struct hfsp_rect - Rectangle coordinates for Finder display
 *
 * Defines a rectangular area used by the Finder for windows
 * and icon positioning in the Mac OS interface.
 *
 * @top: Top edge coordinate
 * @left: Left edge coordinate
 * @bottom: Bottom edge coordinate
 * @right: Right edge coordinate
 */
struct hfsp_rect {
/* 0x00 */
	__be16 top;			/* 0x00 */
	__be16 left;			/* 0x02 */
	__be16 bottom;			/* 0x04 */
	__be16 right;			/* 0x06 */
/* 0x08 */
} __packed;


/*
 * struct DInfo - Directory Finder information (stolen from hfs.h)
 *
 * Contains metadata used by the Mac OS Finder to display directory
 * windows, including position, size, and view settings.
 *
 * @frRect: Directory window rectangle
 * @frFlags: Directory window flags
 * @frLocation: Directory window position
 * @frView: Directory view type (icon, list, etc.)
 */
struct DInfo {
/* 0x00 */
	struct hfsp_rect frRect;	/* 0x00 */
	__be16 frFlags;			/* 0x08 */
	struct hfsp_point frLocation;	/* 0x0A */
	__be16 frView;			/* 0x0E */
/* 0x10 */
} __packed;

/*
 * struct DXInfo - Extended directory Finder information
 *
 * Additional directory display metadata including scroll position
 * and comment references for the Mac OS Finder.
 *
 * @frScroll: Scroll position in directory window
 * @frOpenChain: Linked list of open directory windows
 * @frUnused: Reserved/unused field
 * @frComment: Comment resource ID for directory
 * @frPutAway: Directory ID for "Put Away" command
 */
struct DXInfo {
/* 0x00 */
	struct hfsp_point frScroll;	/* 0x00 */
	__be32 frOpenChain;		/* 0x04 */
	__be16 frUnused;		/* 0x08 */
	__be16 frComment;		/* 0x0A */
	__be32 frPutAway;		/* 0x0C */
/* 0x10 */
} __packed;

/*
 * struct hfsplus_cat_folder - HFS+ catalog record for directories (part of an hfsplus_cat_entry)
 *
 * Complete metadata for a directory stored in the catalog B-tree.
 * Contains directory-specific information including item count,
 * Finder display settings, permissions, and timestamps.
 *
 * @type: Record type (HFSPLUS_FOLDER)
 * @flags: Directory flags
 * @valence: Number of items in directory
 * @id: Catalog Node ID (unique directory ID)
 * @create_date: Directory creation timestamp
 * @content_mod_date: Content modification timestamp
 * @attribute_mod_date: Attribute modification timestamp
 * @access_date: Last access timestamp
 * @backup_date: Last backup timestamp
 * @permissions: POSIX permissions and BSD flags
 * @user_info: Finder display information
 * @finder_info: Extended Finder information
 * @text_encoding: Text encoding hint for filenames
 * @subfolders: Subfolder count (HFSX only, reserved in HFS+)
 */
struct hfsplus_cat_folder {
/* 0x00 */
	__be16 type;				/* 0x00 */
	__be16 flags;				/* 0x02 */
	__be32 valence;				/* 0x04 */
	hfsplus_cnid id;			/* 0x08 */
	__be32 create_date;			/* 0x0C */
	__be32 content_mod_date;		/* 0x10 */
	__be32 attribute_mod_date;		/* 0x14 */
	__be32 access_date;			/* 0x18 */
	__be32 backup_date;			/* 0x1C */
	struct hfsplus_perm permissions;	/* 0x20 */
	struct_group_attr(info, __packed,
		struct DInfo user_info;		/* 0x30 */
		struct DXInfo finder_info;	/* 0x40 */
	);
	__be32 text_encoding;			/* 0x50 */
	__be32 subfolders;			/* 0x54 */
/* 0x58 */
} __packed;

/*
 * struct FInfo - File Finder information (stolen from hfs.h)
 *
 * Contains metadata used by the Mac OS Finder to display and
 * handle files, including type, creator, and display location.
 *
 * @fdType: File type (4-character code)
 * @fdCreator: Creator application (4-character code)
 * @fdFlags: Finder flags (visibility, lock, etc.)
 * @fdLocation: Icon position in folder window
 * @fdFldr: Folder containing the file
 */
struct FInfo {
/* 0x00 */
	__be32 fdType;			/* 0x00 */
	__be32 fdCreator;		/* 0x04 */
	__be16 fdFlags;			/* 0x08 */
	struct hfsp_point fdLocation;	/* 0x0A */
	__be16 fdFldr;			/* 0x0E */
/* 0x10 */
} __packed;

/*
 * struct FXInfo - Extended file Finder information
 *
 * Additional file metadata used by the Mac OS Finder, including
 * custom icon references and comment associations.
 *
 * @fdIconID: Custom icon resource ID
 * @fdUnused: Reserved/unused bytes
 * @fdComment: Comment resource ID
 * @fdPutAway: Directory ID for "Put Away" command
 */
struct FXInfo {
/* 0x00 */
	__be16 fdIconID;		/* 0x00 */
	u8 fdUnused[8];			/* 0x02 */
	__be16 fdComment;		/* 0x0A */
	__be32 fdPutAway;		/* 0x0C */
/* 0x10 */
} __packed;

/*
 * struct hfsplus_cat_file - HFS+ catalog record for files (part of a cat_entry)
 *
 * Complete metadata for a regular file stored in the catalog B-tree.
 * Contains both data and resource fork information, Finder metadata,
 * timestamps, permissions, and extent records for both forks.
 *
 * @type: Record type (HFSPLUS_FILE)
 * @flags: File flags
 * @reserved1: Reserved field
 * @id: Catalog Node ID (unique file ID)
 * @create_date: File creation timestamp
 * @content_mod_date: Content modification timestamp
 * @attribute_mod_date: Attribute modification timestamp
 * @access_date: Last access timestamp
 * @backup_date: Last backup timestamp
 * @permissions: POSIX permissions and BSD flags
 * @user_info: Finder file information
 * @finder_info: Extended Finder information
 * @text_encoding: Text encoding hint for filename
 * @reserved2: Reserved field
 * @data_fork: Data fork information and extents
 * @rsrc_fork: Resource fork information and extents
 */
struct hfsplus_cat_file {
/* 0x00 */
	__be16 type;				/* 0x00 */
	__be16 flags;				/* 0x02 */
	u32 reserved1;				/* 0x04 */
	hfsplus_cnid id;			/* 0x08 */
	__be32 create_date;			/* 0x0C */
	__be32 content_mod_date;		/* 0x10 */
	__be32 attribute_mod_date;		/* 0x14 */
	__be32 access_date;			/* 0x18 */
	__be32 backup_date;			/* 0x1C */
	struct hfsplus_perm permissions;	/* 0x20 */
	struct_group_attr(info, __packed,
		struct FInfo user_info;		/* 0x30 */
		struct FXInfo finder_info;	/* 0x40 */
	);
	__be32 text_encoding;			/* 0x50 */
	u32 reserved2;				/* 0x54 */

	struct hfsplus_fork_raw data_fork;	/* 0x58 */
	struct hfsplus_fork_raw rsrc_fork;	/* 0xA8 */
/* 0xF8 */
} __packed;

/* File and folder flag bits */
#define HFSPLUS_FILE_LOCKED		0x0001
#define HFSPLUS_FILE_THREAD_EXISTS	0x0002
#define HFSPLUS_XATTR_EXISTS		0x0004
#define HFSPLUS_ACL_EXISTS		0x0008
#define HFSPLUS_HAS_FOLDER_COUNT	0x0010	/* Folder has subfolder count
						 * (HFSX only) */

/*
 * struct hfsplus_cat_thread - HFS+ catalog thread record (part of a cat_entry)
 *
 * Thread records provide reverse lookup capability in the catalog
 * B-tree, allowing navigation from a file/directory ID back to its
 * parent directory and name. Essential for hard link support.
 *
 * @type: Record type (HFSPLUS_*_THREAD)
 * @reserved: Reserved field for alignment
 * @parentID: Catalog Node ID of parent directory
 * @nodeName: Unicode name of file/directory
 */
struct hfsplus_cat_thread {
/* 0x000 */
	__be16 type;			/* 0x00 */
	s16 reserved;			/* 0x02 */
	hfsplus_cnid parentID;		/* 0x04 */
	struct hfsplus_unistr nodeName;	/* 0x08 */
/* 0x208 */
} __packed;

#define HFSPLUS_MIN_THREAD_SZ 10

/*
 * union hfsplus_cat_entry - Generic HFS+ catalog record
 *
 * Union representing any type of catalog B-tree record. The first
 * field (type) determines which variant to use: folder, file, or
 * thread record.
 *
 * @type: Record type discriminator
 * @folder: Directory record
 * @file: File record
 * @thread: Thread record
 */
typedef union {
	__be16 type;
	struct hfsplus_cat_folder folder;
	struct hfsplus_cat_file file;
	struct hfsplus_cat_thread thread;
} __packed hfsplus_cat_entry;

/* HFS+ catalog entry type */
#define HFSPLUS_FOLDER         0x0001
#define HFSPLUS_FILE           0x0002
#define HFSPLUS_FOLDER_THREAD  0x0003
#define HFSPLUS_FILE_THREAD    0x0004

/*
 * struct hfsplus_ext_key - HFS+ extents overflow B-tree key
 *
 * Key structure for the extents overflow B-tree, which stores
 * additional extent records when files need more than the 8
 * extents stored in the catalog record.
 *
 * @key_len: Total length of key in bytes
 * @fork_type: Fork type (data=0x00, resource=0xFF)
 * @pad: Padding for alignment
 * @cnid: Catalog Node ID of file
 * @start_block: Starting allocation block number
 */
struct hfsplus_ext_key {
/* 0x00 */
	__be16 key_len;		/* 0x00 */
	u8 fork_type;		/* 0x02 */
	u8 pad;			/* 0x03 */
	hfsplus_cnid cnid;	/* 0x04 */
	__be32 start_block;	/* 0x08 */
/* 0x0C */
} __packed;

#define HFSPLUS_EXT_KEYLEN	sizeof(struct hfsplus_ext_key)

#define HFSPLUS_XATTR_FINDER_INFO_NAME "com.apple.FinderInfo"
#define HFSPLUS_XATTR_ACL_NAME "com.apple.system.Security"

#define HFSPLUS_ATTR_INLINE_DATA 0x10
#define HFSPLUS_ATTR_FORK_DATA   0x20
#define HFSPLUS_ATTR_EXTENTS     0x30

/*
 * struct hfsplus_attr_key - HFS+ attributes B-tree key
 *
 * Key structure for the attributes B-tree, which stores extended
 * attributes (xattrs) for files and directories. Organized by
 * file ID and attribute name.
 *
 * @key_len: Total length of key in bytes
 * @pad: Padding for alignment
 * @cnid: Catalog Node ID of file/directory
 * @start_block: Starting block for extent attributes
 * @key_name: Unicode attribute name
 */
struct hfsplus_attr_key {
/* 0x000 */
	__be16 key_len;				/* 0x00 */
	__be16 pad;				/* 0x02 */
	hfsplus_cnid cnid;			/* 0x04 */
	__be32 start_block;			/* 0x08 */
	struct hfsplus_attr_unistr key_name;	/* 0x0C */
/* 0x10C */
} __packed;

#define HFSPLUS_ATTR_KEYLEN	sizeof(struct hfsplus_attr_key)

/*
 * struct hfsplus_attr_fork_data - HFS+ fork-based attribute
 *
 * Used for large extended attributes that are stored as separate
 * fork structures with their own extent records.
 *
 * @record_type: Record type (HFSPLUS_ATTR_FORK_DATA)
 * @reserved: Reserved field
 * @the_fork: Fork information and extents
 */
struct hfsplus_attr_fork_data {
/* 0x00 */
	__be32 record_type;			/* 0x00 */
	__be32 reserved;			/* 0x04 */
	struct hfsplus_fork_raw the_fork;	/* 0x08 */
/* 0x58 */
} __packed;

/*
 * struct hfsplus_attr_extents - HFS+ attribute extent record
 *
 * Used for extended attributes that require additional extent
 * records beyond what fits in the fork structure.
 *
 * @record_type: Record type (HFSPLUS_ATTR_EXTENTS)
 * @reserved: Reserved field
 * @extents: Additional extent records
 */
struct hfsplus_attr_extents {
/* 0x00 */
	__be32 record_type;		/* 0x00 */
	__be32 reserved;		/* 0x04 */
	struct hfsplus_extent extents;	/* 0x08 */
/* 0x48 */
} __packed;

#define HFSPLUS_MAX_INLINE_DATA_SIZE 3802

/*
 * struct hfsplus_attr_inline_data - HFS+ inline attribute data
 *
 * Used for small extended attributes that can be stored directly
 * within the attributes B-tree record. Most efficient storage
 * method for small attributes.
 *
 * @record_type: Record type (HFSPLUS_ATTR_INLINE_DATA)
 * @reserved1: Reserved field
 * @reserved2: Additional reserved bytes
 * @length: Length of attribute data
 * @raw_bytes: Actual attribute data
 */
struct hfsplus_attr_inline_data {
/* 0x000 */
	__be32 record_type;				/* 0x00 */
	__be32 reserved1;				/* 0x04 */
	u8 reserved2[6];				/* 0x08 */
	__be16 length;					/* 0x0E */
	u8 raw_bytes[HFSPLUS_MAX_INLINE_DATA_SIZE];	/* 0x10 */
/* 0xEEA */
} __packed;

/*
 * union hfsplus_attr_entry - Generic HFS+ attribute record
 *
 * Union representing any type of attributes B-tree record.
 * The first field (record_type) determines which variant to use.
 *
 * @record_type: Record type discriminator
 * @fork_data: Fork-based attribute
 * @extents: Extent-based attribute
 * @inline_data: Inline attribute data
 */
typedef union {
	__be32 record_type;
	struct hfsplus_attr_fork_data fork_data;
	struct hfsplus_attr_extents extents;
	struct hfsplus_attr_inline_data inline_data;
} __packed hfsplus_attr_entry;

/*
 * union hfsplus_btree_key - Generic HFS+ B-tree key
 *
 * Union representing any HFS+ B-tree key type. The first field
 * indicates the key length, and the specific key type is determined
 * by context (catalog, extents, or attributes B-tree).
 *
 * @key_len: Key length (common to all key types)
 * @cat: Catalog B-tree key
 * @ext: Extents B-tree key
 * @attr: Attributes B-tree key
 */
typedef union {
	__be16 key_len;
	struct hfsplus_cat_key cat;
	struct hfsplus_ext_key ext;
	struct hfsplus_attr_key attr;
} __packed hfsplus_btree_key;

#endif
