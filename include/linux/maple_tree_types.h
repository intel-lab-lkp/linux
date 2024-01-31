/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _LINUX_MAPLE_TREE_TYPES_H
#define _LINUX_MAPLE_TREE_TYPES_H
/*
 * Maple Tree - An RCU-safe adaptive tree for storing ranges
 * Copyright (c) 2018-2022 Oracle
 * Authors:     Liam R. Howlett <Liam.Howlett@Oracle.com>
 *              Matthew Wilcox <willy@infradead.org>
 */

#include <linux/spinlock_types.h>
#include <linux/string.h> // for memset()
#include <linux/limits.h> // for ULONG_MAX

/*
 * Allocated nodes are mutable until they have been inserted into the tree,
 * at which time they cannot change their type until they have been removed
 * from the tree and an RCU grace period has passed.
 *
 * Removed nodes have their ->parent set to point to themselves.  RCU readers
 * check ->parent before relying on the value that they loaded from the
 * slots array.  This lets us reuse the slots array for the RCU head.
 *
 * Nodes in the tree point to their parent unless bit 0 is set.
 */
#if defined(CONFIG_64BIT) || defined(BUILD_VDSO32_64)
/* 64bit sizes */
#define MAPLE_NODE_SLOTS	31	/* 256 bytes including ->parent */
#define MAPLE_RANGE64_SLOTS	16	/* 256 bytes */
#define MAPLE_ARANGE64_SLOTS	10	/* 240 bytes */
#define MAPLE_ALLOC_SLOTS	(MAPLE_NODE_SLOTS - 1)
#else
/* 32bit sizes */
#define MAPLE_NODE_SLOTS	63	/* 256 bytes including ->parent */
#define MAPLE_RANGE64_SLOTS	32	/* 256 bytes */
#define MAPLE_ARANGE64_SLOTS	21	/* 240 bytes */
#define MAPLE_ALLOC_SLOTS	(MAPLE_NODE_SLOTS - 2)
#endif /* defined(CONFIG_64BIT) || defined(BUILD_VDSO32_64) */

#define MAPLE_NODE_MASK		255UL

/*
 * The node->parent of the root node has bit 0 set and the rest of the pointer
 * is a pointer to the tree itself.  No more bits are available in this pointer
 * (on m68k, the data structure may only be 2-byte aligned).
 *
 * Internal non-root nodes can only have maple_range_* nodes as parents.  The
 * parent pointer is 256B aligned like all other tree nodes.  When storing a 32
 * or 64 bit values, the offset can fit into 4 bits.  The 16 bit values need an
 * extra bit to store the offset.  This extra bit comes from a reuse of the last
 * bit in the node type.  This is possible by using bit 1 to indicate if bit 2
 * is part of the type or the slot.
 *
 * Once the type is decided, the decision of an allocation range type or a range
 * type is done by examining the immutable tree flag for the MAPLE_ALLOC_RANGE
 * flag.
 *
 *  Node types:
 *   0x??1 = Root
 *   0x?00 = 16 bit nodes
 *   0x010 = 32 bit nodes
 *   0x110 = 64 bit nodes
 *
 *  Slot size and location in the parent pointer:
 *   type  : slot location
 *   0x??1 : Root
 *   0x?00 : 16 bit values, type in 0-1, slot in 2-6
 *   0x010 : 32 bit values, type in 0-2, slot in 3-6
 *   0x110 : 64 bit values, type in 0-2, slot in 3-6
 */

/*
 * This metadata is used to optimize the gap updating code and in reverse
 * searching for gaps or any other code that needs to find the end of the data.
 */
struct maple_metadata {
	unsigned char end;
	unsigned char gap;
};

/*
 * Leaf nodes do not store pointers to nodes, they store user data.  Users may
 * store almost any bit pattern.  As noted above, the optimisation of storing an
 * entry at 0 in the root pointer cannot be done for data which have the bottom
 * two bits set to '10'.  We also reserve values with the bottom two bits set to
 * '10' which are below 4096 (ie 2, 6, 10 .. 4094) for internal use.  Some APIs
 * return errnos as a negative errno shifted right by two bits and the bottom
 * two bits set to '10', and while choosing to store these values in the array
 * is not an error, it may lead to confusion if you're testing for an error with
 * mas_is_err().
 *
 * Non-leaf nodes store the type of the node pointed to (enum maple_type in bits
 * 3-6), bit 2 is reserved.  That leaves bits 0-1 unused for now.
 *
 * In regular B-Tree terms, pivots are called keys.  The term pivot is used to
 * indicate that the tree is specifying ranges,  Pivots may appear in the
 * subtree with an entry attached to the value whereas keys are unique to a
 * specific position of a B-tree.  Pivot values are inclusive of the slot with
 * the same index.
 */

struct maple_range_64 {
	struct maple_pnode *parent;
	unsigned long pivot[MAPLE_RANGE64_SLOTS - 1];
	union {
		void __rcu *slot[MAPLE_RANGE64_SLOTS];
		struct {
			void __rcu *pad[MAPLE_RANGE64_SLOTS - 1];
			struct maple_metadata meta;
		};
	};
};

/*
 * At tree creation time, the user can specify that they're willing to trade off
 * storing fewer entries in a tree in return for storing more information in
 * each node.
 *
 * The maple tree supports recording the largest range of NULL entries available
 * in this node, also called gaps.  This optimises the tree for allocating a
 * range.
 */
struct maple_arange_64 {
	struct maple_pnode *parent;
	unsigned long pivot[MAPLE_ARANGE64_SLOTS - 1];
	void __rcu *slot[MAPLE_ARANGE64_SLOTS];
	unsigned long gap[MAPLE_ARANGE64_SLOTS];
	struct maple_metadata meta;
};

struct maple_alloc {
	unsigned long total;
	unsigned char node_count;
	unsigned int request_count;
	struct maple_alloc *slot[MAPLE_ALLOC_SLOTS];
};

struct maple_topiary {
	struct maple_pnode *parent;
	struct maple_enode *next; /* Overlaps the pivot */
};

enum maple_type {
	maple_dense,
	maple_leaf_64,
	maple_range_64,
	maple_arange_64,
};

#ifdef CONFIG_LOCKDEP
typedef struct lockdep_map *lockdep_map_p;
#else
typedef struct { /* nothing */ } lockdep_map_p;
#endif

/*
 * If the tree contains a single entry at index 0, it is usually stored in
 * tree->ma_root.  To optimise for the page cache, an entry which ends in '00',
 * '01' or '11' is stored in the root, but an entry which ends in '10' will be
 * stored in a node.  Bits 3-6 are used to store enum maple_type.
 *
 * The flags are used both to store some immutable information about this tree
 * (set at tree creation time) and dynamic information set under the spinlock.
 *
 * Another use of flags are to indicate global states of the tree.  This is the
 * case with the MAPLE_USE_RCU flag, which indicates the tree is currently in
 * RCU mode.  This mode was added to allow the tree to reuse nodes instead of
 * re-allocating and RCU freeing nodes when there is a single user.
 */
struct maple_tree {
	union {
		spinlock_t	ma_lock;
		lockdep_map_p	ma_external_lock;
	};
	unsigned int	ma_flags;
	void __rcu      *ma_root;
};

/*
 * The Maple Tree squeezes various bits in at various points which aren't
 * necessarily obvious.  Usually, this is done by observing that pointers are
 * N-byte aligned and thus the bottom log_2(N) bits are available for use.  We
 * don't use the high bits of pointers to store additional information because
 * we don't know what bits are unused on any given architecture.
 *
 * Nodes are 256 bytes in size and are also aligned to 256 bytes, giving us 8
 * low bits for our own purposes.  Nodes are currently of 4 types:
 * 1. Single pointer (Range is 0-0)
 * 2. Non-leaf Allocation Range nodes
 * 3. Non-leaf Range nodes
 * 4. Leaf Range nodes All nodes consist of a number of node slots,
 *    pivots, and a parent pointer.
 */

struct maple_node {
	union {
		struct {
			struct maple_pnode *parent;
			void __rcu *slot[MAPLE_NODE_SLOTS];
		};
		struct {
			void *pad;
			struct rcu_head rcu;
			struct maple_enode *piv_parent;
			unsigned char parent_slot;
			enum maple_type type;
			unsigned char slot_len;
			unsigned int ma_flags;
		};
		struct maple_range_64 mr64;
		struct maple_arange_64 ma64;
		struct maple_alloc alloc;
	};
};

/*
 * More complicated stores can cause two nodes to become one or three and
 * potentially alter the height of the tree.  Either half of the tree may need
 * to be rebalanced against the other.  The ma_topiary struct is used to track
 * which nodes have been 'cut' from the tree so that the change can be done
 * safely at a later date.  This is done to support RCU.
 */
struct ma_topiary {
	struct maple_enode *head;
	struct maple_enode *tail;
	struct maple_tree *mtree;
};

/* Advanced API */

/*
 * Maple State Status
 * ma_active means the maple state is pointing to a node and offset and can
 * continue operating on the tree.
 * ma_start means we have not searched the tree.
 * ma_root means we have searched the tree and the entry we found lives in
 * the root of the tree (ie it has index 0, length 1 and is the only entry in
 * the tree).
 * ma_none means we have searched the tree and there is no node in the
 * tree for this entry.  For example, we searched for index 1 in an empty
 * tree.  Or we have a tree which points to a full leaf node and we
 * searched for an entry which is larger than can be contained in that
 * leaf node.
 * ma_pause means the data within the maple state may be stale, restart the
 * operation
 * ma_overflow means the search has reached the upper limit of the search
 * ma_underflow means the search has reached the lower limit of the search
 * ma_error means there was an error, check the node for the error number.
 */
enum maple_status {
	ma_active,
	ma_start,
	ma_root,
	ma_none,
	ma_pause,
	ma_overflow,
	ma_underflow,
	ma_error,
};

/*
 * The maple state is defined in the struct ma_state and is used to keep track
 * of information during operations, and even between operations when using the
 * advanced API.
 *
 * If state->node has bit 0 set then it references a tree location which is not
 * a node (eg the root).  If bit 1 is set, the rest of the bits are a negative
 * errno.  Bit 2 (the 'unallocated slots' bit) is clear.  Bits 3-6 indicate the
 * node type.
 *
 * state->alloc either has a request number of nodes or an allocated node.  If
 * stat->alloc has a requested number of nodes, the first bit will be set (0x1)
 * and the remaining bits are the value.  If state->alloc is a node, then the
 * node will be of type maple_alloc.  maple_alloc has MAPLE_NODE_SLOTS - 1 for
 * storing more allocated nodes, a total number of nodes allocated, and the
 * node_count in this node.  node_count is the number of allocated nodes in this
 * node.  The scaling beyond MAPLE_NODE_SLOTS - 1 is handled by storing further
 * nodes into state->alloc->slot[0]'s node.  Nodes are taken from state->alloc
 * by removing a node from the state->alloc node until state->alloc->node_count
 * is 1, when state->alloc is returned and the state->alloc->slot[0] is promoted
 * to state->alloc.  Nodes are pushed onto state->alloc by putting the current
 * state->alloc into the pushed node's slot[0].
 *
 * The state also contains the implied min/max of the state->node, the depth of
 * this search, and the offset. The implied min/max are either from the parent
 * node or are 0-oo for the root node.  The depth is incremented or decremented
 * every time a node is walked down or up.  The offset is the slot/pivot of
 * interest in the node - either for reading or writing.
 *
 * When returning a value the maple state index and last respectively contain
 * the start and end of the range for the entry.  Ranges are inclusive in the
 * Maple Tree.
 *
 * The status of the state is used to determine how the next action should treat
 * the state.  For instance, if the status is ma_start then the next action
 * should start at the root of the tree and walk down.  If the status is
 * ma_pause then the node may be stale data and should be discarded.  If the
 * status is ma_overflow, then the last action hit the upper limit.
 *
 */
struct ma_state {
	struct maple_tree *tree;	/* The tree we're operating in */
	unsigned long index;		/* The index we're operating on - range start */
	unsigned long last;		/* The last index we're operating on - range end */
	struct maple_enode *node;	/* The node containing this entry */
	unsigned long min;		/* The minimum index of this node - implied pivot min */
	unsigned long max;		/* The maximum index of this node - implied pivot max */
	struct maple_alloc *alloc;	/* Allocated nodes for this operation */
	enum maple_status status;	/* The status of the state (active, start, none, etc) */
	unsigned char depth;		/* depth of tree descent during write */
	unsigned char offset;
	unsigned char mas_flags;
	unsigned char end;		/* The end of the node */
};

struct ma_wr_state {
	struct ma_state *mas;
	struct maple_node *node;	/* Decoded mas->node */
	unsigned long r_min;		/* range min */
	unsigned long r_max;		/* range max */
	enum maple_type type;		/* mas->node type */
	unsigned char offset_end;	/* The offset where the write ends */
	unsigned long *pivots;		/* mas->node->pivots pointer */
	unsigned long end_piv;		/* The pivot at the offset end */
	void __rcu **slots;		/* mas->node->slots pointer */
	void *entry;			/* The entry to write */
	void *content;			/* The existing entry that is being overwritten */
};

static inline void mas_init(struct ma_state *mas, struct maple_tree *tree,
			    unsigned long addr)
{
	memset(mas, 0, sizeof(struct ma_state));
	mas->tree = tree;
	mas->index = mas->last = addr;
	mas->max = ULONG_MAX;
	mas->status = ma_start;
	mas->node = NULL;
}

#endif /*_LINUX_MAPLE_TREE_TYPES_H */
