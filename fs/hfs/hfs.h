/*
 *  linux/fs/hfs/hfs.h
 *
 * Copyright (C) 1995-1997  Paul H. Hargrove
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 * This file may be distributed under the terms of the GNU General Public License.
 */

#ifndef _HFS_H
#define _HFS_H

/* offsets to various blocks */
#define HFS_DD_BLK		0 /* Driver Descriptor block */
#define HFS_PMAP_BLK		1 /* First block of partition map */
#define HFS_MDB_BLK		2 /* Block (w/i partition) of MDB */

/* magic numbers for various disk blocks */
#define HFS_DRVR_DESC_MAGIC	0x4552 /* "ER": driver descriptor map */
#define HFS_OLD_PMAP_MAGIC	0x5453 /* "TS": old-type partition map */
#define HFS_NEW_PMAP_MAGIC	0x504D /* "PM": new-type partition map */
#define HFS_SUPER_MAGIC		0x4244 /* "BD": HFS MDB (super block) */
#define HFS_MFS_SUPER_MAGIC	0xD2D7 /* MFS MDB (super block) */

/* various FIXED size parameters */
#define HFS_SECTOR_SIZE		512    /* size of an HFS sector */
#define HFS_SECTOR_SIZE_BITS	9      /* log_2(HFS_SECTOR_SIZE) */
#define HFS_NAMELEN		31     /* maximum length of an HFS filename */
#define HFS_MAX_NAMELEN		128
#define HFS_MAX_VALENCE		32767U

/* Meanings of the drAtrb field of the MDB,
 * Reference: _Inside Macintosh: Files_ p. 2-61
 */
#define HFS_SB_ATTRIB_HLOCK	(1 << 7)
#define HFS_SB_ATTRIB_UNMNT	(1 << 8)
#define HFS_SB_ATTRIB_SPARED	(1 << 9)
#define HFS_SB_ATTRIB_INCNSTNT	(1 << 11)
#define HFS_SB_ATTRIB_SLOCK	(1 << 15)

/* Some special File ID numbers */
#define HFS_POR_CNID		1	/* Parent Of the Root */
#define HFS_ROOT_CNID		2	/* ROOT directory */
#define HFS_EXT_CNID		3	/* EXTents B-tree */
#define HFS_CAT_CNID		4	/* CATalog B-tree */
#define HFS_BAD_CNID		5	/* BAD blocks file */
#define HFS_ALLOC_CNID		6	/* ALLOCation file (HFS+) */
#define HFS_START_CNID		7	/* STARTup file (HFS+) */
#define HFS_ATTR_CNID		8	/* ATTRibutes file (HFS+) */
#define HFS_EXCH_CNID		15	/* ExchangeFiles temp id */
#define HFS_FIRSTUSER_CNID	16

/* values for hfs_cat_rec.cdrType */
#define HFS_CDR_DIR    0x01    /* folder (directory) */
#define HFS_CDR_FIL    0x02    /* file */
#define HFS_CDR_THD    0x03    /* folder (directory) thread */
#define HFS_CDR_FTH    0x04    /* file thread */

/* legal values for hfs_ext_key.FkType and hfs_file.fork */
#define HFS_FK_DATA	0x00
#define HFS_FK_RSRC	0xFF

/* bits in hfs_fil_entry.Flags */
#define HFS_FIL_LOCK	0x01  /* locked */
#define HFS_FIL_THD	0x02  /* file thread */
#define HFS_FIL_DOPEN   0x04  /* data fork open */
#define HFS_FIL_ROPEN   0x08  /* resource fork open */
#define HFS_FIL_DIR     0x10  /* directory (always clear) */
#define HFS_FIL_NOCOPY  0x40  /* copy-protected file */
#define HFS_FIL_USED	0x80  /* open */

/* bits in hfs_dir_entry.Flags. dirflags is 16 bits. */
#define HFS_DIR_LOCK        0x01  /* locked */
#define HFS_DIR_THD         0x02  /* directory thread */
#define HFS_DIR_INEXPFOLDER 0x04  /* in a shared area */
#define HFS_DIR_MOUNTED     0x08  /* mounted */
#define HFS_DIR_DIR         0x10  /* directory (always set) */
#define HFS_DIR_EXPFOLDER   0x20  /* share point */

/* bits hfs_finfo.fdFlags */
#define HFS_FLG_INITED		0x0100
#define HFS_FLG_LOCKED		0x1000
#define HFS_FLG_INVISIBLE	0x4000

/*======== HFS structures as they appear on the disk ========*/

/*
 * struct hfs_name - HFS Pascal-style filename
 *
 * HFS uses Pascal-style strings with a length byte followed by
 * the actual characters. Maximum filename length is 31 characters.
 *
 * @len: Length of filename (0-31)
 * @name: Filename characters (not null-terminated)
 */
struct hfs_name {
/* 0x00 */
	u8 len;				/* 0x00 */
	u8 name[HFS_NAMELEN];		/* 0x01 */
/* 0x20 */
} __packed;

/*
 * struct hfs_point - 2D coordinate point for Finder display
 *
 * Used by the Finder for positioning icons and windows.
 *
 * @v: Vertical coordinate
 * @h: Horizontal coordinate
 */
struct hfs_point {
/* 0x00 */
	__be16 v;			/* 0x00 */
	__be16 h;			/* 0x02 */
/* 0x04 */
} __packed;

/*
 * struct hfs_rect - Rectangle coordinates for Finder display
 *
 * Defines a rectangular area used by the Finder for windows
 * and icon positioning.
 *
 * @top: Top edge coordinate
 * @left: Left edge coordinate
 * @bottom: Bottom edge coordinate
 * @right: Right edge coordinate
 */
struct hfs_rect {
/* 0x00 */
	__be16 top;			/* 0x00 */
	__be16 left;			/* 0x02 */
	__be16 bottom;			/* 0x04 */
	__be16 right;			/* 0x06 */
/* 0x08 */
} __packed;

/*
 * struct hfs_finfo - Finder information for files
 *
 * Contains metadata used by the Mac OS Finder to display
 * and handle files, including type, creator, and location.
 *
 * @fdType: File type (4-character code)
 * @fdCreator: Creator application (4-character code)
 * @fdFlags: Finder flags (visibility, lock, etc.)
 * @fdLocation: Icon position in folder window
 * @fdFldr: Folder containing the file
 */
struct hfs_finfo {
/* 0x00 */
	__be32 fdType;			/* 0x00 */
	__be32 fdCreator;		/* 0x04 */
	__be16 fdFlags;			/* 0x08 */
	struct hfs_point fdLocation;	/* 0x0A */
	__be16 fdFldr;			/* 0x0E */
/* 0x10 */
} __packed;

/*
 * struct hfs_fxinfo - Extended Finder information for files
 *
 * Additional metadata used by the Finder, including custom
 * icon references and comment associations.
 *
 * @fdIconID: Custom icon resource ID
 * @fdUnused: Reserved/unused bytes
 * @fdComment: Comment resource ID
 * @fdPutAway: Directory ID for "Put Away" command
 */
struct hfs_fxinfo {
/* 0x00 */
	__be16 fdIconID;		/* 0x00 */
	u8 fdUnused[8];			/* 0x02 */
	__be16 fdComment;		/* 0x0A */
	__be32 fdPutAway;		/* 0x0C */
/* 0x10 */
} __packed;

/*
 * struct hfs_dinfo - Finder information for directories
 *
 * Contains metadata used by the Finder to display directory
 * windows, including position, size, and view settings.
 *
 * @frRect: Directory window rectangle
 * @frFlags: Directory window flags
 * @frLocation: Directory window position
 * @frView: Directory view type (icon, list, etc.)
 */
struct hfs_dinfo {
/* 0x00 */
	struct hfs_rect frRect;		/* 0x00 */
	__be16 frFlags;			/* 0x08 */
	struct hfs_point frLocation;	/* 0x0A */
	__be16 frView;			/* 0x0E */
/* 0x10 */
} __packed;

/*
 * struct hfs_dxinfo - Extended Finder information for directories
 *
 * Additional directory display metadata including scroll position
 * and comment references.
 *
 * @frScroll: Scroll position in directory window
 * @frOpenChain: Linked list of open directory windows
 * @frUnused: Reserved/unused field
 * @frComment: Comment resource ID for directory
 * @frPutAway: Directory ID for "Put Away" command
 */
struct hfs_dxinfo {
/* 0x00 */
	struct hfs_point frScroll;	/* 0x00 */
	__be32 frOpenChain;		/* 0x04 */
	__be16 frUnused;		/* 0x08 */
	__be16 frComment;		/* 0x0A */
	__be32 frPutAway;		/* 0x0C */
/* 0x10 */
} __packed;

/*
 * union hfs_finder_info - Combined Finder metadata
 *
 * Union containing either file or directory Finder information.
 * The type of entry determines which variant to use.
 */
union hfs_finder_info {
/* 0x00 */
	struct {
		struct hfs_finfo finfo;		/* Basic file Finder info */
		struct hfs_fxinfo fxinfo;	/* Extended file Finder info */
	} file;
	struct {
		struct hfs_dinfo dinfo;		/* Basic directory Finder info */
		struct hfs_dxinfo dxinfo;	/* Extended directory Finder info */
	} dir;
/* 0x20 */
} __packed;

/* Cast to a pointer to a generic bkey */
#define	HFS_BKEY(X)	(((void)((X)->KeyLen)), ((struct hfs_bkey *)(X)))

/*
 * struct hfs_cat_key - Catalog B-tree search key
 *
 * Key structure used for searching the catalog B-tree. Entries are
 * organized by parent directory ID and filename, allowing efficient
 * lookups and directory traversals.
 *
 * @key_len: Total length of key in bytes
 * @reserved: Padding byte for alignment
 * @ParID: Catalog Node ID of parent directory
 * @CName: Filename within the parent directory
 */
struct hfs_cat_key {
/* 0x00 */
	u8 key_len;		/* 0x00 */
	u8 reserved;		/* 0x01 */
	__be32 ParID;		/* 0x02 */
	struct hfs_name	CName;	/* 0x06 */
/* 0x26 */
} __packed;

/*
 * struct hfs_ext_key - Extents overflow B-tree search key
 *
 * Key structure for the extents overflow B-tree, which stores
 * additional extent records when files need more than the 3
 * extents stored in the catalog record.
 *
 * @key_len: Total length of key in bytes
 * @FkType: Fork type: HFS_FK_DATA or HFS_FK_RSRC
 * @FNum: Catalog Node ID (File ID) of the file
 * @FABN: Starting allocation block number for extents
 */
struct hfs_ext_key {
/* 0x00 */
	u8 key_len;		/* 0x00 */
	u8 FkType;		/* 0x01 */
	__be32 FNum;		/* 0x02 */
	__be16 FABN;		/* 0x06 */
/* 0x08 */
} __packed;

/*
 * union hfs_btree_key - Generic B-tree key
 *
 * Union representing any HFS B-tree key type. The first byte
 * indicates the key length, and the specific key type is
 * determined by context (catalog or extents B-tree).
 *
 * @key_len: Key length (common to all key types)
 * @cat: Catalog B-tree key
 * @ext: Extents B-tree key
 */
typedef union hfs_btree_key {
/* 0x00 */
	u8 key_len;
	struct hfs_cat_key cat;
	struct hfs_ext_key ext;
/* 0x26 */
} hfs_btree_key;

#define HFS_MAX_CAT_KEYLEN	(sizeof(struct hfs_cat_key) - sizeof(u8))
#define HFS_MAX_EXT_KEYLEN	(sizeof(struct hfs_ext_key) - sizeof(u8))

typedef union hfs_btree_key btree_key;

/*
 * struct hfs_extent - File extent descriptor
 *
 * Describes a contiguous range of allocation blocks belonging
 * to a file. Multiple extents can describe a fragmented file.
 *
 * @block: Starting allocation block number
 * @count: Number of contiguous blocks
 */
struct hfs_extent {
/* 0x00 */
	__be16 block;		/* 0x00 */
	__be16 count;		/* 0x02 */
/* 0x04 */
};

/*
 * hfs_extent_rec - Array of 3 extent descriptors
 *
 * HFS stores up to 3 extent records directly in catalog entries.
 * Additional extents for fragmented files are stored in the
 * extents overflow B-tree.
 */
typedef struct hfs_extent hfs_extent_rec[3];

/*
 * struct hfs_cat_file - Catalog record for regular files
 *
 * Complete metadata for a regular file stored in the catalog B-tree.
 * Contains both data and resource fork information, Finder metadata,
 * timestamps, and the first 3 extent records for each fork.
 *
 * @type: Record type (HFS_CDR_FIL)
 * @reserved: Reserved byte for alignment
 * @Flags: File flags (locked, open, etc.)
 * @Typ: File version number (always 0)
 * @UsrWds: Finder information for file
 * @FlNum: Catalog Node ID (unique file ID)
 * @StBlk: Obsolete: starting block (unused)
 * @LgLen: Logical EOF of data fork (file size)
 * @PyLen: Physical EOF of data fork (disk space)
 * @RStBlk: Obsolete: resource fork start block
 * @RLgLen: Logical EOF of resource fork
 * @RPyLen: Physical EOF of resource fork
 * @CrDat: File creation date/time
 * @MdDat: File modification date/time
 * @BkDat: Last backup date/time
 * @FndrInfo: Extended Finder information
 * @ClpSize: Clump size: bytes to allocate when extending this file
 * @ExtRec: First 3 extent records for data fork
 * @RExtRec: First 3 extent records for resource fork
 * @Resrv: Reserved field for future use
 */
struct hfs_cat_file {
/* 0x00 */
	s8 type;			/* 0x00 */
	u8 reserved;			/* 0x01 */
	u8 Flags;			/* 0x02 */
	s8 Typ;				/* 0x03 */
	struct hfs_finfo UsrWds;	/* 0x04 */
	__be32 FlNum;			/* 0x14 */
	__be16 StBlk;			/* 0x18 */
	__be32 LgLen;			/* 0x1A */
	__be32 PyLen;			/* 0x1E */
	__be16 RStBlk;			/* 0x22 */
	__be32 RLgLen;			/* 0x24 */
	__be32 RPyLen;			/* 0x28 */
	__be32 CrDat;			/* 0x2C */
	__be32 MdDat;			/* 0x30 */
	__be32 BkDat;			/* 0x34 */
	struct hfs_fxinfo FndrInfo;	/* 0x38 */
	__be16 ClpSize;			/* 0x48 */
	hfs_extent_rec ExtRec;		/* 0x4A */
	hfs_extent_rec RExtRec;		/* 0x56 */
	u32 Resrv;			/* 0x62 */
/* 0x66 */
} __packed;

/*
 * struct hfs_cat_dir - Catalog record for directories
 *
 * Complete metadata for a directory stored in the catalog B-tree.
 * Contains directory-specific information including item count,
 * Finder display settings, and timestamps.
 *
 * @type: Record type (HFS_CDR_DIR)
 * @reserved: Reserved byte for alignment
 * @Flags: Directory flags (locked, mounted, etc.)
 * @Val: Valence: total number of items (files + subdirs) in directory
 * @DirID: Catalog Node ID (unique directory ID)
 * @CrDat: Directory creation date/time
 * @MdDat: Directory modification date/time
 * @BkDat: Last backup date/time
 * @UsrInfo: Finder display information
 * @FndrInfo: Extended Finder information
 * @Resrv: Reserved space for future use
 */
struct hfs_cat_dir {
/* 0x00 */
	s8 type;			/* 0x00 */
	u8 reserved;			/* 0x01 */
	__be16 Flags;			/* 0x02 */
	__be16 Val;			/* 0x04 */
	__be32 DirID;			/* 0x06 */
	__be32 CrDat;			/* 0x0A */
	__be32 MdDat;			/* 0x0E */
	__be32 BkDat;			/* 0x12 */
	struct hfs_dinfo UsrInfo;	/* 0x16 */
	struct hfs_dxinfo FndrInfo;	/* 0x26 */
	u8 Resrv[16];			/* 0x36 */
/* 0x46 */
} __packed;

/*
 * struct hfs_cat_thread - Catalog thread record
 *
 * Thread records provide reverse lookup capability in the catalog
 * B-tree, allowing navigation from a file/directory ID back to its
 * parent directory and name. Required for hard link support.
 *
 * @type: Record type (HFS_CDR_THD or HFS_CDR_FTH)
 * @reserved: Reserved bytes for alignment
 * @ParID: Catalog Node ID of parent directory
 * @CName: Name of the file/directory
 */
struct hfs_cat_thread {
/* 0x00 */
	s8 type;			/* 0x00 */
	u8 reserved[9];			/* 0x01 */
	__be32 ParID;			/* 0x0A */
	struct hfs_name CName;		/* 0x0E */
/* 0x2E */
}  __packed;

/*
 * union hfs_cat_rec - Generic catalog record
 *
 * Union representing any type of catalog B-tree record.
 * The first byte (type) determines which variant to use:
 * HFS_CDR_FIL (file), HFS_CDR_DIR (directory),
 * HFS_CDR_THD/HFS_CDR_FTH (thread records).
 */
typedef union hfs_cat_rec {
	s8 type;			/* Record type discriminator */
	struct hfs_cat_file file;	/* File record (type == HFS_CDR_FIL) */
	struct hfs_cat_dir dir;		/* Directory record (type == HFS_CDR_DIR) */
	struct hfs_cat_thread thread;	/* Thread record (type == HFS_CDR_*TH) */
} hfs_cat_rec;

/*
 * struct hfs_mdb - Master Directory Block (HFS Superblock)
 *
 * The Master Directory Block is the primary metadata structure of an HFS
 * filesystem, equivalent to a superblock in other filesystems. It contains
 * essential filesystem parameters, allocation information, B-tree locations,
 * and statistics. Located at block 2 of the HFS partition.
 *
 * @drSigWord: Filesystem signature (HFS_SUPER_MAGIC)
 * @drCrDate: Filesystem creation timestamp
 * @drLsMod: Last modification timestamp
 * @drAtrb: Volume attributes flags
 * @drNmFls: Number of files in root directory
 * @drVBMSt: Starting block of volume bitmap (in 512-byte sectors)
 * @drAllocPtr: Next allocation search start point (in allocation blocks)
 * @drNmAlBlks: Total number of allocation blocks
 * @drAlBlkSiz: Size of each allocation block in bytes
 * @drClpSiz: Default clump size: bytes to allocate when extending files
 * @drAlBlSt: Starting block of allocation area (in 512-byte sectors)
 * @drNxtCNID: Next available Catalog Node ID for new files/directories
 * @drFreeBks: Number of free allocation blocks
 * @drVN: Volume name (Pascal string, 27 chars max)
 * @drVolBkUp: Volume backup timestamp
 * @drVSeqNum: Backup sequence number
 * @drWrCnt: Filesystem write operation counter
 * @drXTClpSiz: Clump size for extents overflow B-tree
 * @drCTClpSiz: Clump size for catalog B-tree
 * @drNmRtDirs: Number of directories in root directory
 * @drFilCnt: Total number of files in filesystem
 * @drDirCnt: Total number of directories in filesystem
 * @drFndrInfo: Finder information and boot parameters
 * @drEmbedSigWord: Signature of embedded volume (if any)
 * @drEmbedExtent: Embedded volume extent: starting block number and block count combined
 * @drXTFlSize: Logical size of extents B-tree file
 * @drXTExtRec: First 3 extent records for extents B-tree
 * @drCTFlSize: Logical size of catalog B-tree file
 * @drCTExtRec: First 3 extent records for catalog B-tree
 */
struct hfs_mdb {
/* 0x00 */
	__be16 drSigWord;		/* 0x00 */
	__be32 drCrDate;		/* 0x02 */
	__be32 drLsMod;			/* 0x06 */
	__be16 drAtrb;			/* 0x0A */
	__be16 drNmFls;			/* 0x0C */
	__be16 drVBMSt;			/* 0x0E */
	__be16 drAllocPtr;		/* 0x10 */
	__be16 drNmAlBlks;		/* 0x12 */
	__be32 drAlBlkSiz;		/* 0x14 */
	__be32 drClpSiz;		/* 0x18 */
	__be16 drAlBlSt;		/* 0x1C */
	__be32 drNxtCNID;		/* 0x1E */
	__be16 drFreeBks;		/* 0x22 */
	u8 drVN[28];			/* 0x24 */
	__be32 drVolBkUp;		/* 0x40 */
	__be16 drVSeqNum;		/* 0x44 */
	__be32 drWrCnt;			/* 0x46 */
	__be32 drXTClpSiz;		/* 0x4A */
	__be32 drCTClpSiz;		/* 0x4E */
	__be16 drNmRtDirs;		/* 0x52 */
	__be32 drFilCnt;		/* 0x54 */
	__be32 drDirCnt;		/* 0x58 */
	u8 drFndrInfo[32];		/* 0x5C */
	__be16 drEmbedSigWord;		/* 0x7C */
	__be32 drEmbedExtent;		/* 0x7E */
	__be32 drXTFlSize;		/* 0x82 */
	hfs_extent_rec drXTExtRec;	/* 0x86 */
	__be32 drCTFlSize;		/* 0x92 */
	hfs_extent_rec drCTExtRec;	/* 0x96 */
/* 0xA2 */
} __packed;

/*======== Data structures kept in memory ========*/

/*
 * struct hfs_readdir_data - Directory reading state
 *
 * Runtime structure used to track the state of directory reading
 * operations. Maintains position and context for readdir() calls
 * to ensure consistent directory traversal.
 *
 * @list: List linkage for multiple readers
 * @file: Associated file descriptor
 * @key: Current position in directory
 */
struct hfs_readdir_data {
	struct list_head list;
	struct file *file;
	struct hfs_cat_key key;
};

#endif
