/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  linux/fs/hfs/btree.h
 *
 * Copyright (C) 2001
 * Brad Boyer (flar@allandria.com)
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 */

#include "hfs_fs.h"

typedef int (*btree_keycmp)(const btree_key *, const btree_key *);

#define NODE_HASH_SIZE  256

/* B-tree mutex nested subclasses */
enum hfs_btree_mutex_classes {
	CATALOG_BTREE_MUTEX,
	EXTENTS_BTREE_MUTEX,
	ATTR_BTREE_MUTEX,
};

/*
 * struct hfs_btree - In-memory B-tree representation
 *
 * This structure represents a complete HFS B-tree in memory, containing
 * both metadata from the on-disk B-tree header and runtime state needed
 * for efficient B-tree operations. HFS uses B-trees for the catalog
 * (directory structure) and extents overflow (file extent records).
 *
 * @sb: Associated superblock
 * @inode: Inode representing the B-tree file
 * @keycmp: Key comparison function pointer
 * @cnid: Catalog Node ID of this B-tree file
 * @root: Node number of the root node
 * @leaf_count: Total number of leaf records
 * @leaf_head: Node number of first leaf node
 * @leaf_tail: Node number of last leaf node
 * @node_count: Total number of nodes in B-tree
 * @free_nodes: Number of unused/available nodes
 * @attributes: B-tree attributes flags
 * @node_size: Size of each B-tree node in bytes
 * @node_size_shift: log2(node_size) for bit shifting
 * @max_key_len: Maximum key length for this B-tree
 * @depth: Height of B-tree (levels from root)
 * @tree_lock: Mutex protecting B-tree operations
 * @pages_per_bnode: Memory pages per B-tree node
 * @hash_lock: Spinlock protecting node hash table
 * @node_hash: Hash table of cached nodes
 * @node_hash_cnt: Number of nodes in hash table
 */
struct hfs_btree {
	struct super_block *sb;
	struct inode *inode;
	btree_keycmp keycmp;

	u32 cnid;
	u32 root;
	u32 leaf_count;
	u32 leaf_head;
	u32 leaf_tail;
	u32 node_count;
	u32 free_nodes;
	u32 attributes;

	unsigned int node_size;
	unsigned int node_size_shift;
	unsigned int max_key_len;
	unsigned int depth;

	//unsigned int map1_size, map_size;
	struct mutex tree_lock;

	unsigned int pages_per_bnode;
	spinlock_t hash_lock;
	struct hfs_bnode *node_hash[NODE_HASH_SIZE];
	int node_hash_cnt;
};

/*
 * struct hfs_bnode - In-memory B-tree node
 *
 * Represents a single B-tree node in memory, containing both the
 * on-disk node metadata and runtime state for caching, locking,
 * and reference counting. Nodes are cached in a hash table for
 * efficient access.
 *
 * @tree: Parent B-tree this node belongs to
 * @prev: Node ID of previous node at this level
 * @this: This node's ID
 * @next: Node ID of next node at this level
 * @parent: Node ID of parent node
 * @num_recs: Number of records in this node
 * @type: Node type (index, leaf, header, map)
 * @height: Height in B-tree (1=leaf, >1=internal)
 * @next_hash: Next node in hash chain
 * @flags: Node state flags (error, new, deleted)
 * @lock_wq: Wait queue for node locking
 * @refcnt: Reference count for memory management
 * @page_offset: Offset within first page
 * @page: Array of memory pages holding node data
 */
struct hfs_bnode {
	struct hfs_btree *tree;

	u32 prev;
	u32 this;
	u32 next;
	u32 parent;

	u16 num_recs;
	u8 type;
	u8 height;

	struct hfs_bnode *next_hash;
	unsigned long flags;
	wait_queue_head_t lock_wq;
	atomic_t refcnt;
	unsigned int page_offset;
	struct page *page[];
};

#define HFS_BNODE_ERROR		0
#define HFS_BNODE_NEW		1
#define HFS_BNODE_DELETED	2

/*
 * struct hfs_find_data - B-tree search operation context
 *
 * This structure maintains the state of a B-tree search operation,
 * tracking the current position, search parameters, and results.
 * Used by the B-tree search and iteration functions to maintain
 * context across multiple calls.
 *
 * @key: Current key at search position
 * @search_key: Key being searched for
 * @tree: B-tree being searched
 * @bnode: Current node in search
 * @record: Current record index within node
 * @keyoffset: Offset of current key
 * @keylength: Length of current key
 * @entryoffset: Offset of current data
 * @entrylength: Length of current data
 */
struct hfs_find_data {
	btree_key *key;
	btree_key *search_key;
	struct hfs_btree *tree;
	struct hfs_bnode *bnode;
	int record;
	int keyoffset, keylength;
	int entryoffset, entrylength;
};


/* btree.c */
extern struct hfs_btree *hfs_btree_open(struct super_block *, u32, btree_keycmp);
extern void hfs_btree_close(struct hfs_btree *);
extern void hfs_btree_write(struct hfs_btree *);
extern int hfs_bmap_reserve(struct hfs_btree *, int);
extern struct hfs_bnode * hfs_bmap_alloc(struct hfs_btree *);
extern void hfs_bmap_free(struct hfs_bnode *node);

/* bnode.c */
extern void hfs_bnode_read(struct hfs_bnode *, void *, int, int);
extern u16 hfs_bnode_read_u16(struct hfs_bnode *, int);
extern u8 hfs_bnode_read_u8(struct hfs_bnode *, int);
extern void hfs_bnode_read_key(struct hfs_bnode *, void *, int);
extern void hfs_bnode_write(struct hfs_bnode *, void *, int, int);
extern void hfs_bnode_write_u16(struct hfs_bnode *, int, u16);
extern void hfs_bnode_write_u8(struct hfs_bnode *, int, u8);
extern void hfs_bnode_clear(struct hfs_bnode *, int, int);
extern void hfs_bnode_copy(struct hfs_bnode *, int,
			   struct hfs_bnode *, int, int);
extern void hfs_bnode_move(struct hfs_bnode *, int, int, int);
extern void hfs_bnode_dump(struct hfs_bnode *);
extern void hfs_bnode_unlink(struct hfs_bnode *);
extern struct hfs_bnode *hfs_bnode_findhash(struct hfs_btree *, u32);
extern struct hfs_bnode *hfs_bnode_find(struct hfs_btree *, u32);
extern void hfs_bnode_unhash(struct hfs_bnode *);
extern void hfs_bnode_free(struct hfs_bnode *);
extern struct hfs_bnode *hfs_bnode_create(struct hfs_btree *, u32);
extern void hfs_bnode_get(struct hfs_bnode *);
extern void hfs_bnode_put(struct hfs_bnode *);

/* brec.c */
extern u16 hfs_brec_lenoff(struct hfs_bnode *, u16, u16 *);
extern u16 hfs_brec_keylen(struct hfs_bnode *, u16);
extern int hfs_brec_insert(struct hfs_find_data *, void *, int);
extern int hfs_brec_remove(struct hfs_find_data *);

/* bfind.c */
extern int hfs_find_init(struct hfs_btree *, struct hfs_find_data *);
extern void hfs_find_exit(struct hfs_find_data *);
extern int __hfs_brec_find(struct hfs_bnode *, struct hfs_find_data *);
extern int hfs_brec_find(struct hfs_find_data *);
extern int hfs_brec_read(struct hfs_find_data *, void *, int);
extern int hfs_brec_goto(struct hfs_find_data *, int);


/*
 * struct hfs_bnode_desc - On-disk B-tree node descriptor
 *
 * This structure appears at the beginning of every B-tree node on disk.
 * It provides essential metadata for navigating the B-tree structure
 * and understanding the node's contents. Fields marked (V) are variable
 * and may change; fields marked (F) are fixed at B-tree creation.
 */
struct hfs_bnode_desc {
	__be32 next;		/* (V) Node ID of next node at same level */
	__be32 prev;		/* (V) Node ID of previous node at same level */
	u8 type;		/* (F) Node type: index/header/map/leaf */
	u8 height;		/* (F) Distance from leaves (leaves=1) */
	__be16 num_recs;	/* (V) Number of records stored in this node */
	u16 reserved;		/* Reserved space for alignment */
} __packed;

#define HFS_NODE_INDEX	0x00	/* An internal (index) node */
#define HFS_NODE_HEADER	0x01	/* The tree header node (node 0) */
#define HFS_NODE_MAP	0x02	/* Holds part of the bitmap of used nodes */
#define HFS_NODE_LEAF	0xFF	/* A leaf (ndNHeight==1) node */

/*
 * struct hfs_btree_header_rec - B-tree header record
 *
 * This structure is stored as the first record in the header node
 * (node 0) of every HFS B-tree. It contains essential metadata about
 * the B-tree structure, organization, and current state. Fields marked
 * (V) are variable and updated during B-tree operations; fields marked
 * (F) are fixed at B-tree creation time.
 */
struct hfs_btree_header_rec {
	__be16 depth;		/* (V) Number of levels in B-tree (root to leaf) */
	__be32 root;		/* (V) Node ID of the root node */
	__be32 leaf_count;	/* (V) Total number of data records in leaves */
	__be32 leaf_head;	/* (V) Node ID of first (leftmost) leaf */
	__be32 leaf_tail;	/* (V) Node ID of last (rightmost) leaf */
	__be16 node_size;	/* (F) Size of each B-tree node in bytes (512) */
	__be16 max_key_len;	/* (F) Maximum key length for index nodes */
	__be32 node_count;	/* (V) Total number of nodes allocated */
	__be32 free_nodes;	/* (V) Number of unused nodes available */
	u16 reserved1;		/* Reserved field for future use */
	__be32 clump_size;	/* (F) Allocation clump size (rarely used) */
	u8 btree_type;		/* (F) Type identifier for this B-tree */
	u8 reserved2;		/* Reserved field for alignment */
	__be32 attributes;	/* (F) B-tree feature flags and attributes */
	u32 reserved3[16];	/* Reserved space for future expansion */
} __packed;

#define BTREE_ATTR_BADCLOSE	0x00000001	/* b-tree not closed properly. not
						   used by hfsplus. */
#define HFS_TREE_BIGKEYS	0x00000002	/* key length is u16 instead of u8.
						   used by hfsplus. */
#define HFS_TREE_VARIDXKEYS	0x00000004	/* variable key length instead of
						   max key length. use din catalog
						   b-tree but not in extents
						   b-tree (hfsplus). */
