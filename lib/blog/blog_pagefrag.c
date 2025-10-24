// SPDX-License-Identifier: GPL-2.0
/*
 * Binary Logging Page Fragment Management
 *
 * Migrated from ceph_san_pagefrag.c with all algorithms preserved
 */

#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/blog/blog_pagefrag.h>

/**
 * blog_pagefrag_init_with_buffer - Initialize pagefrag with an existing buffer
 * @pf: pagefrag allocator to initialize
 * @buffer: pre-allocated buffer to use
 * @size: size of the buffer
 *
 * Return: 0 on success
 */
int blog_pagefrag_init_with_buffer(struct blog_pagefrag *pf, void *buffer, size_t size)
{
	spin_lock_init(&pf->lock);
	pf->pages = NULL; /* No pages allocated, using provided buffer */
	pf->buffer = buffer;
	pf->capacity = size;  /* Store size for bounds checking */
	pf->head = 0;
	pf->active_elements = 0;
	pf->alloc_count = 0;
	pf->last_entry = NULL;
	return 0;
}
EXPORT_SYMBOL(blog_pagefrag_init_with_buffer);

/**
 * blog_pagefrag_reserve - Reserve space in the pagefrag buffer
 * @pf: pagefrag allocator
 * @n: number of bytes to reserve
 *
 * Checks if there is sufficient space and returns the current head offset
 * WITHOUT advancing the head pointer. This allows the caller to write data
 * before making it visible via blog_pagefrag_publish().
 *
 * This is lockless - only one writer per pagefrag (per-task context).
 *
 * Return: offset to reserved memory, or negative error if not enough space
 */
int blog_pagefrag_reserve(struct blog_pagefrag *pf, unsigned int n)
{
	if (pf->head + n > pf->capacity)
		return -ENOMEM; /* No space left */
	return pf->head;  /* Return offset without advancing */
}
EXPORT_SYMBOL(blog_pagefrag_reserve);

/**
 * blog_pagefrag_publish - Publish reserved space by advancing head pointer
 * @pf: pagefrag allocator
 * @publish_head: new head value (offset + bytes_written)
 *
 * Atomically advances the head pointer to make previously written data visible
 * to readers. Must be called after blog_pagefrag_reserve() and writing data.
 *
 * Uses memory barrier to ensure all writes are visible before head is updated.
 * This prevents readers from seeing partially-written entries.
 *
 * This is lockless - only one writer per pagefrag (per-task context).
 */
void blog_pagefrag_publish(struct blog_pagefrag *pf, u64 publish_head)
{
	/* Ensure all prior writes are visible before updating head */
	smp_wmb();

	/* Atomically update head to make data visible to readers */
	pf->head = publish_head;
	pf->alloc_count++;
	pf->active_elements++;
}
EXPORT_SYMBOL(blog_pagefrag_publish);

/**
 * blog_pagefrag_get_ptr - Get buffer pointer from pagefrag reserve result
 * @pf: pagefrag allocator
 * @val: return value from blog_pagefrag_reserve
 *
 * Return: pointer to reserved buffer region
 */
void *blog_pagefrag_get_ptr(struct blog_pagefrag *pf, u64 val)
{
	void *rc = (void *)(pf->buffer + val);

	if (unlikely(pf->pages && pf->buffer != page_address(pf->pages))) {
		pr_err("%s: invalid buffer pointer %llx @ %s\n", __func__,
		       (unsigned long long)pf->buffer, current->comm);
		WARN_ON_ONCE(1);
		return NULL;
	}
	if (unlikely((rc) < pf->buffer || (rc) >= (pf->buffer + pf->capacity))) {
		pr_err("%s: invalid pointer %llx\n", __func__,
		       (unsigned long long)rc);
		WARN_ON_ONCE(1);
		return NULL;
	}
	return rc;
}
EXPORT_SYMBOL(blog_pagefrag_get_ptr);

/**
 * blog_pagefrag_reset - Reset the pagefrag allocator.
 *
 * Resets the head and tail pointers to the beginning of the buffer.
 */
void blog_pagefrag_reset(struct blog_pagefrag *pf)
{
	spin_lock(&pf->lock);
	pf->head = 0;
	pf->active_elements = 0;
	pf->alloc_count = 0;
	pf->last_entry = NULL;
	spin_unlock(&pf->lock);
}
EXPORT_SYMBOL(blog_pagefrag_reset);

