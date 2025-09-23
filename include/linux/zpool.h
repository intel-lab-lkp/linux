/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zpool memory storage api
 *
 * Copyright (C) 2014 Dan Streetman
 *
 * This is a common frontend for the zswap compressed memory storage
 * implementations.
 */

#ifndef _ZPOOL_H_
#define _ZPOOL_H_

struct zpool {
	void *pool;
};

struct zpool *zpool_create_pool(const char *name);
void zpool_destroy_pool(struct zpool *pool);
unsigned long  zpool_malloc(struct zpool *pool, size_t size, gfp_t gfp, const int nid);
void zpool_free(struct zpool *pool, unsigned long handle);
void *zpool_obj_read_begin(struct zpool *zpool, unsigned long handle, void *local_copy);
void zpool_obj_read_end(struct zpool *zpool, unsigned long handle, void *handle_mem);
void zpool_obj_write(struct zpool *zpool, unsigned long handle, void *handle_mem, size_t mem_len);
u64 zpool_get_total_pages(struct zpool *zpool);

#define DECLARE_ZPOOL(prefix) \
	struct zpool *zpool_create_pool(const char *name) \
	{ \
		return (struct zpool *) prefix ## _create_pool(name); \
	} \
	void zpool_destroy_pool(struct zpool *pool) \
	{ \
		return prefix ## _destroy_pool(pool->pool); \
	} \
	unsigned long  zpool_malloc(struct zpool *pool, size_t size, gfp_t gfp, const int nid) \
	{ \
		return prefix ## _malloc(pool->pool, size, gfp, nid); \
	} \
	void zpool_free(struct zpool *pool, unsigned long handle) \
	{ \
		return prefix ## _free(pool->pool, handle); \
	} \
	void *zpool_obj_read_begin(struct zpool *zpool, unsigned long handle, void *local_copy) \
	{ \
		return prefix ## _obj_read_begin(zpool->pool, handle, local_copy); \
	} \
	void zpool_obj_read_end(struct zpool *zpool, unsigned long handle, void *handle_mem) \
	{ \
		return prefix ## _obj_read_end(zpool->pool, handle, handle_mem); \
	} \
	void zpool_obj_write(struct zpool *zpool, unsigned long handle, \
		     void *handle_mem, size_t mem_len) \
	{ \
		prefix ## _obj_write(zpool->pool, handle, handle_mem, mem_len); \
	} \
	u64 zpool_get_total_pages(struct zpool *zpool) \
	{ \
		return prefix ## _get_total_pages(zpool->pool); \
	}

#endif
