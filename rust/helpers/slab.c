// SPDX-License-Identifier: GPL-2.0

#include <linux/slab.h>

void * __must_check __realloc_size(2)
rust_helper_krealloc(const void *objp, size_t new_size, gfp_t flags)
{
	return krealloc(objp, new_size, flags);
}

void * __must_check __realloc_size(2)
rust_helper_kvrealloc(const void *p, size_t size, gfp_t flags)
{
	return kvrealloc(p, size, flags);
}

struct kmem_cache * rust_helper_kmem_cache_create(const char *name, unsigned int size, unsigned int align, gfp_t flags, void (*ctor)(void *))
{
	return kmem_cache_create(name, size, align, flags, NULL);
}

void * rust_helper_kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	return kmem_cache_alloc(cachep, flags);
}