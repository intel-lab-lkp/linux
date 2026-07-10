/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright 2020 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Christian König
 */

#ifndef _TTM_PAGE_POOL_H_
#define _TTM_PAGE_POOL_H_

#include <linux/mmzone.h>
#include <linux/llist.h>
#include <linux/spinlock.h>
#include <linux/list_lru.h>
#include <drm/ttm/ttm_caching.h>

struct device;
struct seq_file;
struct ttm_backup_flags;
struct ttm_operation_ctx;
struct ttm_pool;
struct ttm_tt;

/**
 * struct ttm_pool_type - Pool for a certain memory type
 *
 * @pool: the pool we belong to, might be NULL for the global ones
 * @order: the allocation order our pages have
 * @caching: the caching type our pages have
 * @shrinker_list: our place on the global shrinker list
 * @pages: the lru_list of pages in the pool
 */
struct ttm_pool_type {
	struct ttm_pool *pool;
	unsigned int order;
	enum ttm_caching caching;

	struct list_head shrinker_list;

	struct list_lru pages;
};

/**
 * struct ttm_pool - Pool for all caching and orders
 *
 * @dev: the device we allocate pages for
 * @nid: which numa node to use
 * @alloc_flags: TTM_ALLOCATION_POOL_* flags
 * @caching: pools for each caching/order
 */
struct ttm_pool {
	struct device *dev;
	int nid;

	unsigned int alloc_flags;

	struct {
		struct ttm_pool_type orders[NR_PAGE_ORDERS];
	} caching[TTM_NUM_CACHING_TYPES];
};

int ttm_pool_alloc(struct ttm_pool *pool, struct ttm_tt *tt,
		   struct ttm_operation_ctx *ctx);
void ttm_pool_free(struct ttm_pool *pool, struct ttm_tt *tt);

/**
 * struct ttm_pool_prealloc - Pages preallocated outside the dma-resv lock
 * @pages: Array of @count beneficial-order pages (or fewer if a fill fell
 *         short); each entry is the head page of a 1 << @order chunk.
 * @order: Page order of every preallocated chunk.
 * @caching: CPU caching applied to the pages, so leftovers can be restored
 *           to write-back before being freed.
 * @count: Number of valid entries in @pages.
 * @used: Number of entries already consumed by the pool allocator.
 *
 * Defrag pages are interchangeable, so only a count of beneficial-order chunks
 * is needed. ttm_pool_prealloc_fill() populates this outside the lock and
 * __ttm_pool_alloc() drains it; any unused tail is released by
 * ttm_pool_prealloc_fini().
 */
struct ttm_pool_prealloc {
	struct page **pages;
	unsigned int order;
	enum ttm_caching caching;
	unsigned int count;
	unsigned int used;
};

int ttm_pool_prealloc_fill(struct ttm_pool *pool, enum ttm_caching tt_caching,
			   struct ttm_pool_prealloc *pp, unsigned int count);
void ttm_pool_prealloc_fini(struct ttm_pool *pool,
			    struct ttm_pool_prealloc *pp);
unsigned int ttm_pool_prealloc_order(struct ttm_pool *pool);

void ttm_pool_init(struct ttm_pool *pool, struct device *dev,
		   int nid, unsigned int alloc_flags);
void ttm_pool_fini(struct ttm_pool *pool);

int ttm_pool_debugfs(struct ttm_pool *pool, struct seq_file *m);

void ttm_pool_drop_backed_up(struct ttm_tt *tt);

long ttm_pool_backup(struct ttm_pool *pool, struct ttm_tt *ttm,
		     const struct ttm_backup_flags *flags);
int ttm_pool_restore_and_alloc(struct ttm_pool *pool, struct ttm_tt *tt,
			       const struct ttm_operation_ctx *ctx);

unsigned int ttm_pool_page_order_nodma(struct page *p);

int ttm_pool_mgr_init(unsigned long num_pages);
void ttm_pool_mgr_fini(void);

#endif
