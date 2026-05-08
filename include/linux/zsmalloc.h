/*
 * zsmalloc memory allocator
 *
 * Copyright (C) 2011  Nitin Gupta
 * Copyright (C) 2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the license that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 */

#ifndef _ZS_MALLOC_H_
#define _ZS_MALLOC_H_

#include <linux/types.h>

struct zs_pool_stats {
	/* How many pages were migrated (freed) */
	atomic_long_t pages_compacted;
};

struct zs_pool;
struct scatterlist;

enum zs_push_ret {
	ZS_PUSH_OK = 0,
	ZS_PUSH_FULL,
	ZS_PUSH_FULL_QUEUED,
};

struct zs_deferred_ops {
	enum zs_push_ret (*push)(void *buf, unsigned int count,
					  unsigned long value);
	void (*drain)(void *private, void *buf, unsigned int count);
};

struct zs_pool *zs_create_pool(const char *name);
void zs_destroy_pool(struct zs_pool *pool);

unsigned long zs_malloc(struct zs_pool *pool, size_t size, gfp_t flags,
			const int nid);
void zs_free(struct zs_pool *pool, unsigned long obj);
int zs_pool_enable_deferred_free(struct zs_pool *pool,
				 const struct zs_deferred_ops *ops,
				 void *private);
bool zs_free_deferred(struct zs_pool *pool, unsigned long value);

size_t zs_huge_class_size(struct zs_pool *pool);

unsigned long zs_get_total_pages(struct zs_pool *pool);
unsigned long zs_compact(struct zs_pool *pool);

unsigned int zs_lookup_class_index(struct zs_pool *pool, unsigned int size);

void zs_pool_stats(struct zs_pool *pool, struct zs_pool_stats *stats);

void *zs_obj_read_begin(struct zs_pool *pool, unsigned long handle,
			size_t mem_len, void *local_copy);
void zs_obj_read_end(struct zs_pool *pool, unsigned long handle,
		     size_t mem_len, void *handle_mem);
void zs_obj_read_sg_begin(struct zs_pool *pool, unsigned long handle,
			  struct scatterlist *sg, size_t mem_len);
void zs_obj_read_sg_end(struct zs_pool *pool, unsigned long handle);
void zs_obj_write(struct zs_pool *pool, unsigned long handle,
		  void *handle_mem, size_t mem_len);

extern const struct movable_operations zsmalloc_mops;

#endif
