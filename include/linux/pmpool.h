/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _PMPOOL_H
#define _PMPOOL_H

struct page;

#if defined(CONFIG_PMPOOL)
struct page *pmpool_alloc(unsigned long count);
bool pmpool_release(struct page *pages, unsigned long count);
#else
static inline struct page *pmpool_alloc(unsigned long count)
{
	return NULL;
}
static inline bool pmpool_release(struct page *pages, unsigned long count)
{
	return false;
}
#endif

#endif /* _PMPOOL_H */
