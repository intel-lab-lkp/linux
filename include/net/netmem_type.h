/* SPDX-License-Identifier: GPL-2.0
 *
 *	Author:	Byungchul Park <max.byungchul.park@gmail.com>
 */

#ifndef _NET_NETMEM_TYPE_H
#define _NET_NETMEM_TYPE_H

#include <linux/stddef.h>

struct netmem_desc {
	unsigned long __unused_padding;
	struct_group_tagged(__netmem_desc, actual_data,
		unsigned long pp_magic;
		struct page_pool *pp;
		struct net_iov_area *owner;
		unsigned long dma_addr;
		atomic_long_t pp_ref_count;
	);
};

#endif /* _NET_NETMEM_TYPE_H */
