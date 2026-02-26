/* SPDX-License-Identifier: GPL-2.0 */
/* zpdesc.h: zsmalloc pool memory descriptor
 *
 * Written by Alex Shi <alexs@kernel.org>
 *	      Hyeonggon Yoo <42.hyeyoo@gmail.com>
 */
#ifndef __MM_ZPDESC_H__
#define __MM_ZPDESC_H__

#include <linux/migrate.h>
#include <linux/pagemap.h>

/*
 * struct zpdesc -	Memory descriptor for zsmalloc pool memory.
 * @flags:		Page flags, mostly unused by zsmalloc.
 * @lru:		Indirectly used by page migration.
 * @movable_ops:	Used by page migration.
 * @next:		Next zpdesc in a zspage in zsmalloc pool.
 * @handle:		For huge zspage in zsmalloc pool.
 * @zspage:		Points to the zspage this zpdesc is a part of.
 * @first_obj_offset:	First object offset in zsmalloc pool.
 * @_refcount:		The number of references to this zpdesc.
 * @objcgs:		Array of objcgs pointers that the stored objs
 *			belong to. Overlayed on top of page->memcg_data, and
 *			will always have first bit set if it is a valid pointer.
 *
 * This struct overlays struct page for now. Do not modify without a good
 * understanding of the issues.
 *
 * Page flags used:
 * * PG_private identifies the first component page.
 * * PG_locked is used by page migration code.
 */
struct zpdesc {
	unsigned long flags;
	struct list_head lru;
	unsigned long movable_ops;
	union {
		struct zpdesc *next;
		unsigned long handle;
	};
	struct zspage *zspage;
	/*
	 * Only the lower 24 bits are available for offset, limiting a page
	 * to 16 MiB. The upper 8 bits are reserved for PGTY_zsmalloc.
	 *
	 * Do not access this field directly.
	 * Instead, use {get,set}_first_obj_offset() helpers.
	 */
	unsigned int first_obj_offset;
	atomic_t _refcount;
#ifdef CONFIG_MEMCG
	unsigned long objcgs;
#endif
};
#define ZPDESC_MATCH(pg, zp) \
	static_assert(offsetof(struct page, pg) == offsetof(struct zpdesc, zp))

ZPDESC_MATCH(flags, flags);
ZPDESC_MATCH(lru, lru);
ZPDESC_MATCH(mapping, movable_ops);
ZPDESC_MATCH(__folio_index, next);
ZPDESC_MATCH(__folio_index, handle);
ZPDESC_MATCH(private, zspage);
ZPDESC_MATCH(page_type, first_obj_offset);
ZPDESC_MATCH(_refcount, _refcount);
#ifdef CONFIG_MEMCG
ZPDESC_MATCH(memcg_data, objcgs);
#endif
#undef ZPDESC_MATCH
static_assert(sizeof(struct zpdesc) <= sizeof(struct page));

/*
 * zpdesc_page - The first struct page allocated for a zpdesc
 * @zp: The zpdesc.
 *
 * A convenience wrapper for converting zpdesc to the first struct page of the
 * underlying folio, to communicate with code not yet converted to folio or
 * struct zpdesc.
 *
 */
#define zpdesc_page(zp)			(_Generic((zp),			\
	const struct zpdesc *:		(const struct page *)(zp),	\
	struct zpdesc *:		(struct page *)(zp)))

/**
 * zpdesc_folio - The folio allocated for a zpdesc
 * @zp: The zpdesc.
 *
 * Zpdescs are descriptors for zsmalloc memory. The memory itself is allocated
 * as folios that contain the zsmalloc objects, and zpdesc uses specific
 * fields in the first struct page of the folio - those fields are now accessed
 * by struct zpdesc.
 *
 * It is occasionally necessary convert to back to a folio in order to
 * communicate with the rest of the mm. Please use this helper function
 * instead of casting yourself, as the implementation may change in the future.
 */
#define zpdesc_folio(zp)		(_Generic((zp),			\
	const struct zpdesc *:		(const struct folio *)(zp),	\
	struct zpdesc *:		(struct folio *)(zp)))
/**
 * page_zpdesc - Converts from first struct page to zpdesc.
 * @p: The first (either head of compound or single) page of zpdesc.
 *
 * A temporary wrapper to convert struct page to struct zpdesc in situations
 * where we know the page is the compound head, or single order-0 page.
 *
 * Long-term ideally everything would work with struct zpdesc directly or go
 * through folio to struct zpdesc.
 *
 * Return: The zpdesc which contains this page
 */
#define page_zpdesc(p)			(_Generic((p),			\
	const struct page *:		(const struct zpdesc *)(p),	\
	struct page *:			(struct zpdesc *)(p)))

static inline void zpdesc_lock(struct zpdesc *zpdesc)
{
	folio_lock(zpdesc_folio(zpdesc));
}

static inline bool zpdesc_trylock(struct zpdesc *zpdesc)
{
	return folio_trylock(zpdesc_folio(zpdesc));
}

static inline void zpdesc_unlock(struct zpdesc *zpdesc)
{
	folio_unlock(zpdesc_folio(zpdesc));
}

static inline void zpdesc_wait_locked(struct zpdesc *zpdesc)
{
	folio_wait_locked(zpdesc_folio(zpdesc));
}

static inline void zpdesc_get(struct zpdesc *zpdesc)
{
	folio_get(zpdesc_folio(zpdesc));
}

static inline void zpdesc_put(struct zpdesc *zpdesc)
{
	folio_put(zpdesc_folio(zpdesc));
}

static inline void *kmap_local_zpdesc(struct zpdesc *zpdesc)
{
	return kmap_local_page(zpdesc_page(zpdesc));
}

static inline unsigned long zpdesc_pfn(struct zpdesc *zpdesc)
{
	return page_to_pfn(zpdesc_page(zpdesc));
}

static inline struct zpdesc *pfn_zpdesc(unsigned long pfn)
{
	return page_zpdesc(pfn_to_page(pfn));
}

static inline void __zpdesc_set_movable(struct zpdesc *zpdesc)
{
	SetPageMovableOps(zpdesc_page(zpdesc));
}

static inline void __zpdesc_set_zsmalloc(struct zpdesc *zpdesc)
{
	__SetPageZsmalloc(zpdesc_page(zpdesc));
}

static inline struct zone *zpdesc_zone(struct zpdesc *zpdesc)
{
	return page_zone(zpdesc_page(zpdesc));
}

static inline bool zpdesc_is_locked(struct zpdesc *zpdesc)
{
	return folio_test_locked(zpdesc_folio(zpdesc));
}

#ifdef CONFIG_MEMCG
static inline struct obj_cgroup **zpdesc_objcgs(struct zpdesc *zpdesc)
{
	return (struct obj_cgroup **)(zpdesc->objcgs & ~OBJEXTS_FLAGS_MASK);
}

static inline void zpdesc_set_objcgs(struct zpdesc *zpdesc,
				     struct obj_cgroup **objcgs)
{
	zpdesc->objcgs = (unsigned long)objcgs | MEMCG_DATA_OBJEXTS;
}
#endif
#endif
