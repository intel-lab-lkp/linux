// SPDX-License-Identifier: GPL-2.0-only
/*
 * zblock.c
 *
 * Author: Vitaly Wool <vitaly.wool@konsulko.com>
 * Based on the work from Ananda Badmaev <a.badmaev@clicknet.pro>
 * Copyright (C) 2022-2024, Konsulko AB.
 *
 * Zblock is a small object allocator with the intention to serve as a
 * zpool backend. It operates on page blocks which consist of number
 * of physical pages being a power of 2 and store integer number of
 * compressed pages per block which results in determinism and simplicity.
 *
 * zblock doesn't export any API and is meant to be used via zpool API.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/zpool.h>

#define SLOT_FREE 0
#define BIT_SLOT_OCCUPIED 0
#define BIT_SLOT_MAPPED 1

#define SLOT_BITS 5
#define MAX_SLOTS (1 << SLOT_BITS)
#define SLOT_MASK ((0x1UL << SLOT_BITS) - 1)

#define ZBLOCK_HEADER_SIZE	round_up(sizeof(struct zblock_block), sizeof(long))
#define BLOCK_DATA_SIZE(order) ((PAGE_SIZE << order) - ZBLOCK_HEADER_SIZE)
#define SLOT_SIZE(nslots, order) (round_down((BLOCK_DATA_SIZE(order) / nslots), sizeof(long)))

#define BLOCK_CACHE_SIZE 32

struct zblock_pool;

/**
 * struct zblock_block - block metadata
 * Block consists of several (1/2/4/8) pages and contains fixed
 * integer number of slots for allocating compressed pages.
 *
 * free_slots:	number of free slots in the block
 * slot_info:	contains data about free/occupied slots
 * cache_idx:	index of the block in cache
 */
struct zblock_block {
	atomic_t free_slots;
	u64 slot_info[1];
	int cache_idx;
};

/**
 * struct block_desc - general metadata for block lists
 * Each block list stores only blocks of corresponding type which means
 * that all blocks in it have the same number and size of slots.
 * All slots are aligned to size of long.
 *
 * slot_size:		size of slot for this list
 * slots_per_block:	number of slots per block for this list
 * order:		order for __get_free_pages
 */
static const struct block_desc {
	const unsigned int slot_size;
	const unsigned short slots_per_block;
	const unsigned short order;
} block_desc[] = {
	{ SLOT_SIZE(32, 0), 32, 0 },
	{ SLOT_SIZE(22, 0), 22, 0 },
	{ SLOT_SIZE(17, 0), 17, 0 },
	{ SLOT_SIZE(13, 0), 13, 0 },
	{ SLOT_SIZE(11, 0), 11, 0 },
	{ SLOT_SIZE(9, 0), 9, 0 },
	{ SLOT_SIZE(8, 0), 8, 0 },
	{ SLOT_SIZE(14, 1), 14, 1 },
	{ SLOT_SIZE(12, 1), 12, 1 },
	{ SLOT_SIZE(11, 1), 11, 1 },
	{ SLOT_SIZE(10, 1), 10, 1 },
	{ SLOT_SIZE(9, 1), 9, 1 },
	{ SLOT_SIZE(8, 1), 8, 1 },
	{ SLOT_SIZE(15, 2), 15, 2 },
	{ SLOT_SIZE(14, 2), 14, 2 },
	{ SLOT_SIZE(13, 2), 13, 2 },
	{ SLOT_SIZE(12, 2), 12, 2 },
	{ SLOT_SIZE(11, 2), 11, 2 },
	{ SLOT_SIZE(10, 2), 10, 2 },
	{ SLOT_SIZE(9, 2), 9, 2 },
	{ SLOT_SIZE(8, 2), 8, 2 },
	{ SLOT_SIZE(15, 3), 15, 3 },
	{ SLOT_SIZE(14, 3), 14, 3 },
	{ SLOT_SIZE(13, 3), 13, 3 },
	{ SLOT_SIZE(12, 3), 12, 3 },
	{ SLOT_SIZE(11, 3), 11, 3 },
	{ SLOT_SIZE(10, 3), 10, 3 },
	{ SLOT_SIZE(9, 3), 9, 3 },
	{ SLOT_SIZE(7, 3), 7, 3 }
};

/**
 * struct block_list - stores metadata of particular list
 * lock:		protects block_cache
 * block_cache:		blocks with free slots
 * block_count:		total number of blocks in the list
 */
struct block_list {
	spinlock_t lock;
	struct zblock_block *block_cache[BLOCK_CACHE_SIZE];
	unsigned long block_count;
};

/**
 * struct zblock_pool - stores metadata for each zblock pool
 * @block_lists:	array of block lists
 * @zpool:		zpool driver
 * @alloc_flag:		protects block allocation from memory leak
 *
 * This structure is allocated at pool creation time and maintains metadata
 * for a particular zblock pool.
 */
struct zblock_pool {
	struct block_list block_lists[ARRAY_SIZE(block_desc)];
	struct zpool *zpool;
	atomic_t alloc_flag;
};

/*****************
 * Helpers
 *****************/

static int cache_insert_block(struct zblock_block *block, struct block_list *list)
{
	unsigned int i, min_free_slots = atomic_read(&block->free_slots);
	int min_index = -1;

	if (WARN_ON(block->cache_idx != -1))
		return -EINVAL;

	min_free_slots = atomic_read(&block->free_slots);
	for (i = 0; i < BLOCK_CACHE_SIZE; i++) {
		if (!list->block_cache[i] || !atomic_read(&(list->block_cache[i])->free_slots)) {
			min_index = i;
			break;
		}
		if (atomic_read(&(list->block_cache[i])->free_slots) < min_free_slots) {
			min_free_slots = atomic_read(&(list->block_cache[i])->free_slots);
			min_index = i;
		}
	}
	if (min_index >= 0) {
		if (list->block_cache[min_index])
			(list->block_cache[min_index])->cache_idx = -1;
		list->block_cache[min_index] = block;
		block->cache_idx = min_index;
	}
	return min_index < 0 ? min_index : 0;
}

static struct zblock_block *cache_find_block(struct block_list *list)
{
	int i;
	struct zblock_block *z = NULL;

	for (i = 0; i < BLOCK_CACHE_SIZE; i++) {
		if (list->block_cache[i] &&
		    atomic_dec_if_positive(&list->block_cache[i]->free_slots) >= 0) {
			z = list->block_cache[i];
			break;
		}
	}
	return z;
}

static int cache_remove_block(struct block_list *list, struct zblock_block *block)
{
	int idx = block->cache_idx;

	block->cache_idx = -1;
	if (idx >= 0)
		list->block_cache[idx] = NULL;
	return idx < 0 ? idx : 0;
}

/*
 * Encodes the handle of a particular slot in the pool using metadata
 */
static inline unsigned long metadata_to_handle(struct zblock_block *block,
							unsigned int block_type, unsigned int slot)
{
	return (unsigned long)(block) + (block_type << SLOT_BITS) + slot;
}

/* Returns block, block type and slot in the pool corresponding to handle */
static inline struct zblock_block *handle_to_metadata(unsigned long handle,
						unsigned int *block_type, unsigned int *slot)
{
	*block_type = (handle & (PAGE_SIZE - 1)) >> SLOT_BITS;
	*slot = handle & SLOT_MASK;
	return (struct zblock_block *)(handle & PAGE_MASK);
}


/*
 * allocate new block and add it to corresponding block list
 */
static struct zblock_block *alloc_block(struct zblock_pool *pool,
					int block_type, gfp_t gfp,
					unsigned long *handle)
{
	struct zblock_block *block;
	struct block_list *list;

	block = (void *)__get_free_pages(gfp, block_desc[block_type].order);
	if (!block)
		return NULL;

	list = &(pool->block_lists)[block_type];

	/* init block data  */
	memset(&block->slot_info, 0, sizeof(block->slot_info));
	atomic_set(&block->free_slots, block_desc[block_type].slots_per_block - 1);
	block->cache_idx = -1;
	set_bit(BIT_SLOT_OCCUPIED, (unsigned long *)block->slot_info);
	*handle = metadata_to_handle(block, block_type, 0);

	spin_lock(&list->lock);
	cache_insert_block(block, list);
	list->block_count++;
	spin_unlock(&list->lock);
	return block;
}

/*****************
 * API Functions
 *****************/
/**
 * zblock_create_pool() - create a new zblock pool
 * @gfp:	gfp flags when allocating the zblock pool structure
 * @ops:	user-defined operations for the zblock pool
 *
 * Return: pointer to the new zblock pool or NULL if the metadata allocation
 * failed.
 */
static struct zblock_pool *zblock_create_pool(gfp_t gfp)
{
	struct zblock_pool *pool;
	struct block_list *list;
	int i, j;

	pool = kmalloc(sizeof(struct zblock_pool), gfp);
	if (!pool)
		return NULL;

	/* init each block list */
	for (i = 0; i < ARRAY_SIZE(block_desc); i++) {
		list = &(pool->block_lists)[i];
		spin_lock_init(&list->lock);
		for (j = 0; j < BLOCK_CACHE_SIZE; j++)
			list->block_cache[j] = NULL;
		list->block_count = 0;
	}
	atomic_set(&pool->alloc_flag, 0);
	return pool;
}

/**
 * zblock_destroy_pool() - destroys an existing zblock pool
 * @pool:	the zblock pool to be destroyed
 *
 */
static void zblock_destroy_pool(struct zblock_pool *pool)
{
	kfree(pool);
}


/**
 * zblock_alloc() - allocates a slot of appropriate size
 * @pool:	zblock pool from which to allocate
 * @size:	size in bytes of the desired allocation
 * @gfp:	gfp flags used if the pool needs to grow
 * @handle:	handle of the new allocation
 *
 * Return: 0 if success and handle is set, otherwise -EINVAL if the size or
 * gfp arguments are invalid or -ENOMEM if the pool was unable to allocate
 * a new slot.
 */
static int zblock_alloc(struct zblock_pool *pool, size_t size, gfp_t gfp,
			unsigned long *handle)
{
	unsigned int block_type, slot;
	struct zblock_block *block;
	struct block_list *list;

	if (!size)
		return -EINVAL;

	if (size > PAGE_SIZE)
		return -ENOSPC;

	/* find basic block type with suitable slot size */
	for (block_type = 0; block_type < ARRAY_SIZE(block_desc); block_type++) {
		if (size <= block_desc[block_type].slot_size)
			break;
	}
	list = &(pool->block_lists[block_type]);

check:
	/* check if there are free slots in cache */
	spin_lock(&list->lock);
	block = cache_find_block(list);
	spin_unlock(&list->lock);
	if (block)
		goto found;

	/* not found block with free slots try to allocate new empty block */
	if (atomic_cmpxchg(&pool->alloc_flag, 0, 1))
		goto check;
	block = alloc_block(pool, block_type, gfp, handle);
	atomic_set(&pool->alloc_flag, 0);
	if (block)
		return 0;
	return -ENOMEM;

found:
	/* find the first free slot in block */
	for (slot = 0; slot < block_desc[block_type].slots_per_block; slot++) {
		if (!test_and_set_bit(slot*2 + BIT_SLOT_OCCUPIED,
				     (unsigned long *)&block->slot_info))
			break;
	}
	*handle = metadata_to_handle(block, block_type, slot);
	return 0;
}

/**
 * zblock_free() - frees the allocation associated with the given handle
 * @pool:	pool in which the allocation resided
 * @handle:	handle associated with the allocation returned by zblock_alloc()
 *
 */
static void zblock_free(struct zblock_pool *pool, unsigned long handle)
{
	unsigned int slot, block_type;
	struct zblock_block *block;
	struct block_list *list;

	block = handle_to_metadata(handle, &block_type, &slot);
	list = &(pool->block_lists[block_type]);

	spin_lock(&list->lock);
	/* if all slots in block are empty delete whole block */
	if (atomic_inc_return(&block->free_slots) == block_desc[block_type].slots_per_block) {
		list->block_count--;
		cache_remove_block(list, block);
		spin_unlock(&list->lock);
		free_pages((unsigned long)block, block_desc[block_type].order);
		return;
	}

	if (atomic_read(&block->free_slots) < block_desc[block_type].slots_per_block/2
			&& block->cache_idx == -1)
		cache_insert_block(block, list);
	spin_unlock(&list->lock);

	clear_bit(slot*2 + BIT_SLOT_OCCUPIED, (unsigned long *)block->slot_info);
}

/**
 * zblock_map() - maps the allocation associated with the given handle
 * @pool:	pool in which the allocation resides
 * @handle:	handle associated with the allocation to be mapped
 *
 *
 * Returns: a pointer to the mapped allocation
 */
static void *zblock_map(struct zblock_pool *pool, unsigned long handle)
{
	unsigned int block_type, slot;
	struct zblock_block *block;
	void *p;

	block = handle_to_metadata(handle, &block_type, &slot);
	p = (void *)block + ZBLOCK_HEADER_SIZE + slot * block_desc[block_type].slot_size;
	return p;
}

/**
 * zblock_unmap() - unmaps the allocation associated with the given handle
 * @pool:	pool in which the allocation resides
 * @handle:	handle associated with the allocation to be unmapped
 */
static void zblock_unmap(struct zblock_pool *pool, unsigned long handle)
{
}

/**
 * zblock_get_total_pages() - gets the zblock pool size in pages
 * @pool:	pool being queried
 *
 * Returns: size in bytes of the given pool.
 */
static u64 zblock_get_total_pages(struct zblock_pool *pool)
{
	u64 total_size;
	int i;

	total_size = 0;
	for (i = 0; i < ARRAY_SIZE(block_desc); i++)
		total_size += pool->block_lists[i].block_count << block_desc[i].order;

	return total_size;
}

/*****************
 * zpool
 ****************/

static void *zblock_zpool_create(const char *name, gfp_t gfp)
{
	return zblock_create_pool(gfp);
}

static void zblock_zpool_destroy(void *pool)
{
	zblock_destroy_pool(pool);
}

static int zblock_zpool_malloc(void *pool, size_t size, gfp_t gfp,
			unsigned long *handle)
{
	return zblock_alloc(pool, size, gfp, handle);
}

static void zblock_zpool_free(void *pool, unsigned long handle)
{
	zblock_free(pool, handle);
}

static void *zblock_zpool_map(void *pool, unsigned long handle,
			enum zpool_mapmode mm)
{
	return zblock_map(pool, handle);
}

static void zblock_zpool_unmap(void *pool, unsigned long handle)
{
	zblock_unmap(pool, handle);
}

static u64 zblock_zpool_total_pages(void *pool)
{
	return zblock_get_total_pages(pool);
}

static struct zpool_driver zblock_zpool_driver = {
	.type =		"zblock",
	.owner =	THIS_MODULE,
	.create =	zblock_zpool_create,
	.destroy =	zblock_zpool_destroy,
	.malloc =	zblock_zpool_malloc,
	.free =		zblock_zpool_free,
	.map =		zblock_zpool_map,
	.unmap =	zblock_zpool_unmap,
	.total_pages =	zblock_zpool_total_pages,
};

MODULE_ALIAS("zpool-zblock");

static int __init init_zblock(void)
{
	pr_info("loaded\n");
	zpool_register_driver(&zblock_zpool_driver);
	return 0;
}

static void __exit exit_zblock(void)
{
	zpool_unregister_driver(&zblock_zpool_driver);
	pr_info("unloaded\n");
}

module_init(init_zblock);
module_exit(exit_zblock);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vitaly Wool <vitaly.wool@konsulko.com>");
MODULE_DESCRIPTION("Block allocator for compressed pages");
