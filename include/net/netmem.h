/* SPDX-License-Identifier: GPL-2.0
 *
 * netmem.h
 *	Author:	Mina Almasry <almasrymina@google.com>
 *	Copyright (C) 2023 Google LLC
 */

#ifndef _NET_NETMEM_H
#define _NET_NETMEM_H

struct netmem {
	union {
		struct page page;

		/* Stub to prevent compiler implicitly converting from page*
		 * to netmem_t* and vice versa.
		 *
		 * Other memory type(s) net stack would like to support
		 * can be added to this union.
		 */
		void *addr;
	};
};

static inline struct page *netmem_to_page(struct netmem *netmem)
{
	return &netmem->page;
}

static inline struct netmem *page_to_netmem(struct page *page)
{
	return (struct netmem *)page;
}

#endif /* _NET_NETMEM_H */
