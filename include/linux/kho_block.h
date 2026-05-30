/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_KHO_BLOCK_H
#define _LINUX_KHO_BLOCK_H

#include <linux/list.h>
#include <linux/types.h>
#include <linux/kho/abi/block.h>

/**
 * struct kho_block - Internal representation of a serialization block.
 * @list: List head for linking blocks in memory.
 * @ser:  Pointer to the serialized header in preserved memory.
 */
struct kho_block {
	struct list_head list;
	struct kho_block_header_ser *ser;
};

/**
 * struct kho_block_set - A set of blocks that belong to the same object.
 * @blocks:          The list of serialization blocks (struct kho_block).
 * @nblocks:         The number of allocated serialization blocks.
 * @head_pa:         Physical address of the first block header.
 * @entry_size:      The size of each entry in the blocks.
 * @count_per_block: The maximum number of entries each block can hold.
 * @incoming:        True if this block set was restored from the previous kernel.
 */
struct kho_block_set {
	struct list_head blocks;
	long nblocks;
	u64 head_pa;
	size_t entry_size;
	u64 count_per_block;
	bool incoming;
};

/**
 * struct kho_block_it - Iterator for serializing entries into blocks.
 * @bs:         The block set being iterated.
 * @block:      The current block.
 * @i:          The current entry index within @block.
 */
struct kho_block_it {
	struct kho_block_set *bs;
	struct kho_block *block;
	u64 i;
};

/**
 * KHO_BLOCK_SET_INIT - Initialize a static kho_block_set.
 * @_name:       Name of the kho_block_set variable.
 * @_entry_size: The size of each entry in the block set.
 */
#define KHO_BLOCK_SET_INIT(_name, _entry_size) {                        \
	.blocks = LIST_HEAD_INIT((_name).blocks),                       \
	.entry_size = _entry_size,                                      \
}

void kho_block_set_init(struct kho_block_set *bs, size_t entry_size);

int kho_block_grow(struct kho_block_set *bs, u64 count);
void kho_block_shrink(struct kho_block_set *bs, u64 count);

int kho_block_restore(struct kho_block_set *bs, u64 head_pa);
void kho_block_destroy(struct kho_block_set *bs);
void kho_block_set_clear(struct kho_block_set *bs);

void kho_block_it_init(struct kho_block_it *it, struct kho_block_set *bs);
void *kho_block_it_next(struct kho_block_it *it);
void *kho_block_it_read(struct kho_block_it *it);
void *kho_block_it_prev(struct kho_block_it *it);
void kho_block_it_finalize(struct kho_block_it *it);

#endif /* _LINUX_KHO_BLOCK_H */
