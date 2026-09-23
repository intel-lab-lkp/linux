/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mm-internal API for mempolicy.c. Public API lives in
 * include/linux/mempolicy.h.
 */
#ifndef __MM_MEMPOLICY_H
#define __MM_MEMPOLICY_H

#include <linux/gfp.h>
#include <linux/mempolicy.h>

#include "page_alloc.h"

#ifdef CONFIG_NUMA
struct folio *__folio_alloc_mpol_noprof(gfp_t gfp, unsigned int order,
		struct mempolicy *pol, pgoff_t ilx, int nid,
		unsigned int alloc_flags);
#else
static inline struct folio *__folio_alloc_mpol_noprof(gfp_t gfp,
		unsigned int order, struct mempolicy *pol, pgoff_t ilx, int nid,
		unsigned int alloc_flags)
{
	return __folio_alloc_flags_noprof(gfp, order, numa_node_id(), NULL,
					 alloc_flags);
}
#endif

#endif
