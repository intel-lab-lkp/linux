// SPDX-License-Identifier: GPL-2.0

#include <linux/vmalloc.h>

void * __must_check __realloc_size(2)
rust_helper_vrealloc(const void *p, size_t size, unsigned long align, gfp_t flags)
{
	return vrealloc_node(p, size, align, flags, NUMA_NO_NODE);
}
