// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: KHO Serialization Blocks
 *
 * KHO provides a mechanism to preserve stateful data across a kexec handover
 * by serializing it into memory blocks. This file provides the common
 * infrastructure for managing these blocks.
 *
 * Each block consists of a header (struct kho_block_header_ser) followed by an
 * array of serialized entries. Multiple blocks are linked together via a
 * physical pointer in the header, forming a linked list that can be easily
 * traversed in both the current and the next kernel.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/block.h>
#include <linux/kho_block.h>
#include <linux/slab.h>

/*
 * Safeguard limit for the number of serialization blocks. This is used to
 * prevent infinite loops and excessive memory allocation in case of memory
 * corruption in the preserved state.
 */
#define KHO_MAX_BLOCKS 10000

/**
 * kho_block_set_init - Initialize a block set.
 * @bs:         The block set to initialize.
 * @entry_size: The size of each entry in the blocks.
 */
void kho_block_set_init(struct kho_block_set *bs, size_t entry_size)
{
	*bs = (struct kho_block_set)KHO_BLOCK_SET_INIT(*bs, entry_size);
}

static inline u64 kho_block_count_per_block(struct kho_block_set *bs)
{
	if (unlikely(!bs->count_per_block)) {
		bs->count_per_block = (KHO_BLOCK_SIZE -
				       sizeof(struct kho_block_header_ser)) /
				      bs->entry_size;
		WARN_ON(!bs->count_per_block);
	}
	return bs->count_per_block;
}

/* Free serialized data */
static void kho_block_free_ser(struct kho_block_set *bs,
			       struct kho_block_header_ser *ser)
{
	if (bs->incoming)
		kho_restore_free(ser);
	else
		kho_unpreserve_free(ser);
}

static struct kho_block_header_ser *kho_block_alloc_ser(struct kho_block_set *bs)
{
	WARN_ON(bs->incoming);
	return kho_alloc_preserve(KHO_BLOCK_SIZE);
}

static int kho_block_add(struct kho_block_set *bs,
			 struct kho_block_header_ser *ser)
{
	struct kho_block *block, *last;

	if (bs->nblocks >= KHO_MAX_BLOCKS)
		return -ENOSPC;

	block = kzalloc_obj(*block);
	if (!block)
		return -ENOMEM;

	block->ser = ser;
	last = list_last_entry_or_null(&bs->blocks, struct kho_block, list);
	list_add_tail(&block->list, &bs->blocks);
	bs->nblocks++;

	if (last)
		last->ser->next = virt_to_phys(ser);
	else
		bs->head_pa = virt_to_phys(ser);

	return 0;
}

/**
 * kho_block_grow - Create a new block if the current capacity is reached.
 * @bs:    The block set.
 * @count: The current number of entries.
 *
 * This function handles the dynamic expansion of a block set. It allocates
 * and links a new serialization block if the provided entry count matches
 * the current total capacity of the set.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
int kho_block_grow(struct kho_block_set *bs, u64 count)
{
	struct kho_block_header_ser *ser;
	int err;

	if (WARN_ON(bs->incoming))
		return -EINVAL;

	if (count != bs->nblocks * kho_block_count_per_block(bs))
		return 0;

	ser = kho_block_alloc_ser(bs);
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	err = kho_block_add(bs, ser);
	if (err) {
		kho_block_free_ser(bs, ser);
		return err;
	}

	return 0;
}

/**
 * kho_block_shrink - Conditionally destroy the last block in a block set.
 * @bs:              The block set.
 * @count:           The current number of entries across all blocks.
 *
 * This function checks if the last block in the set is redundant based on the
 * total entry count and the capacity of the preceding blocks. If the entry
 * count can be accommodated by the blocks that come before the last one, the
 * last block is destroyed and removed from the set.
 */
void kho_block_shrink(struct kho_block_set *bs, u64 count)
{
	struct kho_block *last, *new_last;

	if (count > (bs->nblocks - 1) * kho_block_count_per_block(bs))
		return;

	if (list_empty(&bs->blocks))
		return;

	last = list_last_entry(&bs->blocks, struct kho_block, list);
	list_del(&last->list);
	bs->nblocks--;
	kho_block_free_ser(bs, last->ser);
	kfree(last);

	new_last = list_last_entry_or_null(&bs->blocks, struct kho_block, list);
	if (new_last)
		new_last->ser->next = 0;
	else
		bs->head_pa = 0;
}

/*
 * kho_cyclic_blocks_check - Check for cycles in a linked list of blocks.
 * Uses Floyd's cycle-finding algorithm to ensure sanity of the incoming list.
 */
static bool kho_cyclic_blocks_check(struct kho_block_set *bs)
{
	struct kho_block_header_ser *fast;
	struct kho_block_header_ser *slow;
	int count = 0;

	fast = phys_to_virt(bs->head_pa);
	slow = fast;

	while (fast) {
		if (count++ >= KHO_MAX_BLOCKS) {
			pr_err("Linked list too long\n");
			return false;
		}

		if (!fast->next)
			break;

		fast = phys_to_virt(fast->next);
		if (!fast->next)
			break;

		fast = phys_to_virt(fast->next);
		slow = phys_to_virt(slow->next);

		if (slow == fast) {
			pr_err("Cyclic list detected\n");
			return false;
		}
	}

	return true;
}

/**
 * kho_block_restore - Restore a block set from a physical address.
 * @bs:      The block set to restore.
 * @head_pa: Physical address of the first block header.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
int kho_block_restore(struct kho_block_set *bs, u64 head_pa)
{
	struct kho_block_header_ser *ser;
	u64 next_pa = head_pa;
	int err;

	/* Restored block sets use size from the previous kernel */
	bs->incoming = true;
	if (!head_pa)
		return 0;

	bs->head_pa = head_pa;
	if (!kho_cyclic_blocks_check(bs)) {
		bs->head_pa = 0;
		return -EINVAL;
	}

	while (next_pa) {
		ser = phys_to_virt(next_pa);
		if (ser->count > kho_block_count_per_block(bs)) {
			pr_warn("Block contains too many entries: %llu\n",
				ser->count);
			err = -EINVAL;
			goto err_destroy;
		}
		err = kho_block_add(bs, ser);
		if (err)
			goto err_destroy;
		next_pa = ser->next;
	}

	return 0;

err_destroy:
	kho_block_destroy(bs);
	return err;
}

/**
 * kho_block_destroy - Destroy all blocks in a block set.
 * @bs:          The block set.
 */
void kho_block_destroy(struct kho_block_set *bs)
{
	u64 head_pa = bs->head_pa;
	struct kho_block *block;

	while (!list_empty(&bs->blocks)) {
		block = list_first_entry(&bs->blocks, struct kho_block, list);
		list_del(&block->list);
		kfree(block);
	}
	bs->nblocks = 0;
	bs->head_pa = 0;

	while (head_pa) {
		struct kho_block_header_ser *ser = phys_to_virt(head_pa);

		head_pa = ser->next;
		kho_block_free_ser(bs, ser);
	}
}

/**
 * kho_block_set_clear - Clear all serialized data in a block set.
 * @bs: The block set to clear.
 */
void kho_block_set_clear(struct kho_block_set *bs)
{
	struct kho_block *block;

	list_for_each_entry(block, &bs->blocks, list) {
		block->ser->count = 0;
		memset(block->ser + 1, 0, KHO_BLOCK_SIZE - sizeof(*block->ser));
	}
}

/**
 * kho_block_it_init - Initialize a block set iterator.
 * @it:         The iterator to initialize.
 * @bs:         The block set to iterate over.
 */
void kho_block_it_init(struct kho_block_it *it, struct kho_block_set *bs)
{
	it->bs = bs;
	it->block = list_first_entry_or_null(&bs->blocks, struct kho_block, list);
	it->i = 0;
}

/**
 * kho_block_it_next - Return the next entry slot in the block set.
 * @it: The block iterator.
 *
 * If the current block is full, it automatically advances to the next block
 * in the set.
 *
 * Return: A pointer to the next entry slot, or NULL if no more slots are
 * available.
 */
void *kho_block_it_next(struct kho_block_it *it)
{
	if (!it->block)
		return NULL;

	if (it->i == kho_block_count_per_block(it->bs)) {
		it->block->ser->count = it->i;
		if (list_is_last(&it->block->list, &it->bs->blocks))
			return NULL;
		it->block = list_next_entry(it->block, list);
		it->i = 0;
	}

	return (void *)(it->block->ser + 1) + (it->i++ * it->bs->entry_size);
}

/**
 * kho_block_it_read - Return the next entry slot for reading.
 * @it: The block iterator.
 *
 * This function iterates through entries that were previously serialized,
 * respecting the count stored in each block's header.
 *
 * Return: A pointer to the next entry slot, or NULL if no more entries are
 * available.
 */
void *kho_block_it_read(struct kho_block_it *it)
{
	if (!it->block)
		return NULL;

	while (it->i == it->block->ser->count) {
		if (list_is_last(&it->block->list, &it->bs->blocks))
			return NULL;
		it->block = list_next_entry(it->block, list);
		it->i = 0;
	}

	return (void *)(it->block->ser + 1) + (it->i++ * it->bs->entry_size);
}

/**
 * kho_block_it_prev - Return the previous entry slot in the block set.
 * @it: The block iterator.
 *
 * If the current index is at the start of a block, it automatically moves to
 * the end of the previous block.
 *
 * Return: A pointer to the previous entry slot, or NULL if at the very
 * beginning of the block set.
 */
void *kho_block_it_prev(struct kho_block_it *it)
{
	if (!it->block)
		return NULL;

	if (it->i == 0) {
		if (list_is_first(&it->block->list, &it->bs->blocks))
			return NULL;
		it->block = list_prev_entry(it->block, list);
		it->i = kho_block_count_per_block(it->bs);
	}

	return (void *)(it->block->ser + 1) + (--it->i * it->bs->entry_size);
}

/**
 * kho_block_it_finalize - Finalize the current block by setting its entry count.
 * @it: The block iterator.
 */
void kho_block_it_finalize(struct kho_block_it *it)
{
	if (it->block)
		it->block->ser->count = it->i;
}
