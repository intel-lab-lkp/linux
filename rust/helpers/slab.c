// SPDX-License-Identifier: GPL-2.0

#include <linux/slab.h>

void * __must_check __realloc_size(2)
rust_helper_krealloc_node(const void *objp, size_t new_size, unsigned long align, gfp_t flags,
			  int nid)
{
	return krealloc_node(objp, new_size, flags, nid);
}

void * __must_check __realloc_size(2)
rust_helper_kvrealloc_node(const void *p, size_t size, unsigned long align, gfp_t flags, int nid)
{
	return kvrealloc_node(p, size, flags, nid);
}
