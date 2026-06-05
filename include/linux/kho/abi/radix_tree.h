/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Google LLC, Jason Miu <jasonmiu@google.com>
 * Copyright (C) 2026 Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_KHO_ABI_RADIX_TREE_H
#define _LINUX_KHO_ABI_RADIX_TREE_H

#include <linux/types.h>
#include <asm/page.h>

/**
 * DOC: KHO persistent memory tracker
 *
 * Subsystems using the KHO persistent memory tracker rely on the stable
 * Application Binary Interface defined below to pass serialized state from a
 * pre-update kernel to a post-update kernel.
 *
 * This interface is a contract. Any modification to the structure fields,
 * compatible strings, or the layout of the serialization structures defined
 * here constitutes a breaking change. Such changes require incrementing the
 * version number in the `KHO_FDT_COMPATIBLE` string to prevent a new kernel
 * from misinterpreting data from an old kernel.
 *
 * Changes are allowed provided the compatibility version is incremented;
 * however, backward/forward compatibility is only guaranteed for kernels
 * supporting the same ABI version.
 *
 * KHO tracks preserved memory using a radix tree data structure. Each node of
 * the tree is exactly a single page. The leaf nodes are bitmaps where each set
 * bit is a preserved page of any order. The intermediate nodes are tables of
 * physical addresses that point to a lower level node.
 *
 * The tree hierarchy is shown below::
 *
 *   root
 *   +-------------------+
 *   |     Level 5       | (struct kho_radix_node)
 *   +-------------------+
 *     |
 *     v
 *   +-------------------+
 *   |     Level 4       | (struct kho_radix_node)
 *   +-------------------+
 *     |
 *     | ... (intermediate levels)
 *     |
 *     v
 *   +-------------------+
 *   |      Level 0      | (struct kho_radix_leaf)
 *   +-------------------+
 *
 * The tree is traversed using a key that encodes the page's physical address
 * (pa) and its order into a single unsigned long value. The encoded key value
 * is composed of two parts: the 'order bit' in the upper part and the
 * 'shifted physical address' in the lower part.::
 *
 *   +------------+-----------------------------+--------------------------+
 *   | Page Order | Order Bit                   | Shifted Physical Address |
 *   +------------+-----------------------------+--------------------------+
 *   | 0          | ...000100 ... (at bit 52)   | pa >> (PAGE_SHIFT + 0)   |
 *   | 1          | ...000010 ... (at bit 51)   | pa >> (PAGE_SHIFT + 1)   |
 *   | 2          | ...000001 ... (at bit 50)   | pa >> (PAGE_SHIFT + 2)   |
 *   | ...        | ...                         | ...                      |
 *   +------------+-----------------------------+--------------------------+
 *
 * Shifted Physical Address:
 * The 'shifted physical address' is the physical address normalized for its
 * order. It effectively represents the PFN shifted right by the order.
 *
 * Order Bit:
 * The 'order bit' encodes the page order by setting a single bit at a
 * specific position. The position of this bit itself represents the order.
 *
 * For instance, on a 64-bit system with 4KB pages (PAGE_SHIFT = 12), the
 * maximum range for the shifted physical address (for order 0) is 52 bits
 * (64 - 12). This address occupies bits [0-51]. For order 0, the order bit is
 * set at position 52.
 *
 * The following diagram illustrates how the encoded key value is split into
 * indices for the tree levels, with PAGE_SIZE of 4KB::
 *
 *        63:60   59:51    50:42    41:33    32:24    23:15         14:0
 *   +---------+--------+--------+--------+--------+--------+-----------------+
 *   |    0    |  Lv 5  |  Lv 4  |  Lv 3  |  Lv 2  |  Lv 1  |  Lv 0 (bitmap)  |
 *   +---------+--------+--------+--------+--------+--------+-----------------+
 *
 * The radix tree stores pages of all orders in a single 6-level hierarchy. It
 * efficiently shares higher tree levels, especially due to common zero top
 * address bits, allowing a single, efficient algorithm to manage all
 * pages. This bitmap approach also offers memory efficiency; for example, a
 * 512KB bitmap can cover a 16GB memory range for 0-order pages with PAGE_SIZE =
 * 4KB.
 */

/*
 * Defines constants for the KHO radix tree structure, used to track preserved
 * memory. These constants govern the indexing, sizing, and depth of the tree.
 */
enum kho_radix_consts {
	/*
	 * The bit position of the order bit (and also the length of the
	 * shifted physical address) for an order-0 page.
	 */
	KHO_ORDER_0_LOG2 = 64 - PAGE_SHIFT,

	/* Size of the table in kho_radix_node, in log2 */
	KHO_TABLE_SIZE_LOG2 = const_ilog2(PAGE_SIZE / sizeof(phys_addr_t)),

	/* Number of bits in the kho_radix_leaf bitmap, in log2 */
	KHO_BITMAP_SIZE_LOG2 = PAGE_SHIFT + const_ilog2(BITS_PER_BYTE),

	/*
	 * The total tree depth is the number of intermediate levels
	 * and 1 bitmap level.
	 */
	KHO_TREE_MAX_DEPTH =
		DIV_ROUND_UP(KHO_ORDER_0_LOG2 - KHO_BITMAP_SIZE_LOG2 + 1,
			     KHO_TABLE_SIZE_LOG2) + 1,
};

struct kho_radix_node {
	u64 table[1 << KHO_TABLE_SIZE_LOG2];
};

struct kho_radix_leaf {
	DECLARE_BITMAP(bitmap, 1 << KHO_BITMAP_SIZE_LOG2);
};

#endif /* _LINUX_KHO_ABI_RADIX_TREE_H */
