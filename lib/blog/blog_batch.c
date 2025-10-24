// SPDX-License-Identifier: GPL-2.0
/*
 * Binary Logging Batch Management
 *
 * Migrated from ceph_san_batch.c with all algorithms preserved
 * Implements per-CPU magazine-based batching for efficient object recycling
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/blog/blog_batch.h>
#include <linux/blog/blog.h>

/* Number of magazines to preallocate during initialization */
#define BLOG_INIT_MAGAZINES 4

static struct blog_magazine *alloc_magazine(struct blog_batch *batch)
{
	struct blog_magazine *mag;

	/* Allocate magazine structure from cache */
	mag = kmem_cache_zalloc(batch->magazine_cache, GFP_KERNEL);
	if (!mag)
		return NULL;

	INIT_LIST_HEAD(&mag->list);
	mag->count = 0;
	return mag;
}

static void free_magazine(struct blog_batch *batch, struct blog_magazine *mag)
{
	int i;
	struct blog_tls_pagefrag *composite;

	/* Free all composites in this magazine before freeing magazine itself */
	for (i = 0; i < mag->count; i++) {
		composite = mag->elements[i];
		if (composite) {
			/* Composites are allocated with alloc_pages(), free with __free_pages() */
			__free_pages(virt_to_page(composite),
				     get_order(BLOG_TLS_PAGEFRAG_ALLOC_SIZE));
		}
	}

	/* Free the magazine structure itself */
	kmem_cache_free(batch->magazine_cache, mag);
}

/**
 * blog_batch_init - Initialize the batching system
 * @batch: Batch structure to initialize
 * @prepopulate: If true, pre-populate magazines with composites
 *
 * Allocates and initializes the per-CPU magazines and global pools.
 * Magazine structures are allocated from a slab cache.
 * Composites (512KB each) are allocated via alloc_pages().
 *
 * If prepopulate is true (typically for alloc_batch), pre-allocates
 * magazines filled with composites. log_batch should start empty.
 *
 * Return: 0 on success, negative error code on failure
 */
int blog_batch_init(struct blog_batch *batch, struct kmem_cache *mag_cache,
		    bool prepopulate)
{
	int cpu, i, j;
	struct blog_cpu_magazine *cpu_mag;
	struct blog_magazine *mag;
	struct blog_tls_pagefrag *composite;
	struct page *pages;

	/* Initialize counters */
	batch->nr_full = 0;
	batch->nr_empty = 0;

	/* Use caller-provided cache or create one */
	if (mag_cache) {
		batch->magazine_cache = mag_cache;
		batch->external_cache = true;
	} else {
		batch->magazine_cache = kmem_cache_create("blog_magazine",
						       sizeof(struct blog_magazine),
						       0, SLAB_HWCACHE_ALIGN, NULL);
		if (!batch->magazine_cache)
			return -ENOMEM;
		batch->external_cache = false;
	}

	/* Initialize global magazine lists */
	INIT_LIST_HEAD(&batch->full_magazines);
	INIT_LIST_HEAD(&batch->empty_magazines);
	spin_lock_init(&batch->full_lock);
	spin_lock_init(&batch->empty_lock);

	/* Allocate per-CPU magazines */
	batch->cpu_magazines = alloc_percpu(struct blog_cpu_magazine);
	if (!batch->cpu_magazines)
		goto cleanup_cache;

	/* Initialize per-CPU magazines to NULL (magazines allocated on-demand) */
	for_each_possible_cpu(cpu) {
		cpu_mag = per_cpu_ptr(batch->cpu_magazines, cpu);
		cpu_mag->mag = NULL;
	}

	/* Pre-populate magazines if requested (typically for alloc_batch) */
	if (prepopulate) {
		for (i = 0; i < BLOG_INIT_MAGAZINES; i++) {
			mag = alloc_magazine(batch);
			if (!mag)
				goto cleanup;

			/* Populate magazine with composites allocated via alloc_pages() */
			for (j = 0; j < BLOG_MAGAZINE_SIZE; j++) {
				/* Allocate 512KB composite via page allocator */
				pages = alloc_pages(GFP_KERNEL | __GFP_ZERO,
						    get_order(BLOG_TLS_PAGEFRAG_ALLOC_SIZE));
				if (!pages) {
					/* Free partially filled magazine */
					free_magazine(batch, mag);
					goto cleanup;
				}
				composite = page_address(pages);
				/* Composite is zeroed - ctx->id == 0, will be initialized on first use */
				mag->elements[j] = composite;
				mag->count++;
			}

			/* Add full magazine to the full pool */
			spin_lock(&batch->full_lock);
			list_add(&mag->list, &batch->full_magazines);
			batch->nr_full++;
			spin_unlock(&batch->full_lock);
		}
	}

	return 0;

cleanup:
	blog_batch_cleanup(batch);
	return -ENOMEM;

cleanup_cache:
	if (!batch->external_cache && batch->magazine_cache)
		kmem_cache_destroy(batch->magazine_cache);
	return -ENOMEM;
}
EXPORT_SYMBOL(blog_batch_init);

/**
 * blog_batch_cleanup - Clean up the batching system
 * @batch: Batch structure to clean up
 *
 * Frees all magazines and composites, and destroys the magazine cache.
 */
void blog_batch_cleanup(struct blog_batch *batch)
{
	int cpu;
	struct blog_magazine *mag, *tmp;
	struct blog_cpu_magazine *cpu_mag;

	/* Free per-CPU magazines */
	if (batch->cpu_magazines) {
		for_each_possible_cpu(cpu) {
			cpu_mag = per_cpu_ptr(batch->cpu_magazines, cpu);
			if (cpu_mag->mag)
				free_magazine(batch, cpu_mag->mag);
		}
		free_percpu(batch->cpu_magazines);
	}

	/* Free magazines in the full pool */
	spin_lock(&batch->full_lock);
	list_for_each_entry_safe(mag, tmp, &batch->full_magazines, list) {
		list_del(&mag->list);
		batch->nr_full--;
		free_magazine(batch, mag);
	}
	spin_unlock(&batch->full_lock);

	/* Free magazines in the empty pool */
	spin_lock(&batch->empty_lock);
	list_for_each_entry_safe(mag, tmp, &batch->empty_magazines, list) {
		list_del(&mag->list);
		batch->nr_empty--;
		free_magazine(batch, mag);
	}
	spin_unlock(&batch->empty_lock);

	/* Destroy magazine cache */
	if (!batch->external_cache && batch->magazine_cache)
		kmem_cache_destroy(batch->magazine_cache);

	batch->magazine_cache = NULL;
	batch->external_cache = false;
}
EXPORT_SYMBOL(blog_batch_cleanup);

/**
 * blog_batch_get - Get an element from the batch
 * @batch: Batch to get element from
 *
 * Return: Element from the magazine, or NULL if none available
 */
void *blog_batch_get(struct blog_batch *batch)
{
	struct blog_cpu_magazine *cpu_mag;
	struct blog_magazine *old_mag, *new_mag;
	void *element = NULL;

	preempt_disable();
	cpu_mag = this_cpu_ptr(batch->cpu_magazines);

	/* If we have a magazine and it has elements, use it */
	if (cpu_mag->mag && cpu_mag->mag->count > 0) {
		element = cpu_mag->mag->elements[--cpu_mag->mag->count];
		goto out;
	}

	/* Current magazine is empty, try to get a full one */
	old_mag = cpu_mag->mag;

	/* Return old magazine to empty pool if we have one */
	if (old_mag) {
		spin_lock(&batch->empty_lock);
		list_add(&old_mag->list, &batch->empty_magazines);
		batch->nr_empty++;
		spin_unlock(&batch->empty_lock);
		cpu_mag->mag = NULL;
	}

	if (batch->nr_full > 0) {
		/* Try to get a full magazine */
		spin_lock(&batch->full_lock);
		if (!list_empty(&batch->full_magazines)) {
			new_mag = list_first_entry(&batch->full_magazines,
						   struct blog_magazine, list);
			list_del(&new_mag->list);
			batch->nr_full--;
			spin_unlock(&batch->full_lock);

			cpu_mag->mag = new_mag;
			if (new_mag->count > 0)
				element = new_mag->elements[--new_mag->count];
		} else {
			spin_unlock(&batch->full_lock);
		}
	}
out:
	preempt_enable();
	return element;
}
EXPORT_SYMBOL(blog_batch_get);

/**
 * blog_batch_put - Put an element back into the batch
 * @batch: Batch to put element into
 * @element: Element to put back
 */
void blog_batch_put(struct blog_batch *batch, void *element)
{
	struct blog_cpu_magazine *cpu_mag;
	struct blog_magazine *mag;

	preempt_disable();
	cpu_mag = this_cpu_ptr(batch->cpu_magazines);

	/* Optimistically try to add to current magazine */
	if (likely(cpu_mag->mag && cpu_mag->mag->count < BLOG_MAGAZINE_SIZE)) {
		cpu_mag->mag->elements[cpu_mag->mag->count++] = element;
		goto out;
	}

	/* If current magazine is full, move it to full pool */
	if (likely(cpu_mag->mag && cpu_mag->mag->count >= BLOG_MAGAZINE_SIZE)) {
		spin_lock(&batch->full_lock);
		list_add_tail(&cpu_mag->mag->list, &batch->full_magazines);
		batch->nr_full++;
		spin_unlock(&batch->full_lock);
		cpu_mag->mag = NULL;
	}

	/* Get new magazine if needed */
	if (likely(!cpu_mag->mag)) {
		/* Try to get from empty pool first */
		spin_lock(&batch->empty_lock);
		if (!list_empty(&batch->empty_magazines)) {
			mag = list_first_entry(&batch->empty_magazines,
					       struct blog_magazine, list);
			list_del(&mag->list);
			batch->nr_empty--;
			spin_unlock(&batch->empty_lock);
			cpu_mag->mag = mag;
		} else {
			spin_unlock(&batch->empty_lock);
			cpu_mag->mag = alloc_magazine(batch);
		}

		if (unlikely(!cpu_mag->mag))
			goto out;
	}
	/* Add element to magazine */
	cpu_mag->mag->elements[cpu_mag->mag->count++] = element;
out:
	preempt_enable();
}
