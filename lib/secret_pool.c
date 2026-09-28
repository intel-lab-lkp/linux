// SPDX-License-Identifier: GPL-2.0
#include <linux/crash_memaction.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/secret_pool.h>
#include <linux/slab.h>

static kmem_buckets * secret_pool __ro_after_init;

static int __init secret_pool_init(void)
{
	secret_pool = kmem_buckets_create("secret", 0, 0, 0, NULL);

	return 0;
}
core_initcall(secret_pool_init);

void *secret_pool_alloc_node(size_t size, gfp_t flags, int node)
{
	void *p = kmem_buckets_alloc_node_track_caller(secret_pool, size,
						       flags, node);

	crash_memaction_mark(p, size, CRASH_MEMACTION_SECRET);

	return p;
}
EXPORT_SYMBOL_GPL(secret_pool_alloc_node);
