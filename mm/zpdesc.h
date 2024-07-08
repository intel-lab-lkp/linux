/* SPDX-License-Identifier: GPL-2.0 */
/* zpdesc.h: zswap.zpool memory descriptor
 *
 * Written by Alex Shi (Tencent) <alexs@kernel.org>
 *	      Hyeonggon Yoo <42.hyeyoo@gmail.com>
 */
#ifndef __MM_ZPDESC_H__
#define __MM_ZPDESC_H__

/*
 * struct zpdesc -	Memory descriptor for zpool memory, now is for zsmalloc
 * @flags:		Page flags, PG_private: identifies the first component page
 * @lru:		Indirectly used by page migration
 * @mops:		Used by page migration
 * @next:		Next zpdesc in a zspage in zsmalloc zpool
 * @handle:		For huge zspage in zsmalloc zpool
 * @zspage:		Pointer to zspage in zsmalloc
 * @first_obj_offset:	First object offset in zsmalloc zpool
 * @_refcount:		Indirectly use by page migration
 * @memcg_data:		Memory Control Group data.
 *
 * This struct overlays struct page for now. Do not modify without a good
 * understanding of the issues.
 */
struct zpdesc {
	unsigned long flags;
	struct list_head lru;
	struct movable_operations *mops;
	union {
		/* Next zpdescs in a zspage in zsmalloc zpool */
		struct zpdesc *next;
		/* For huge zspage in zsmalloc zpool */
		unsigned long handle;
	};
	struct zspage *zspage;
	unsigned int first_obj_offset;
	atomic_t _refcount;
#ifdef CONFIG_MEMCG
	unsigned long memcg_data;
#endif
};
#define ZPDESC_MATCH(pg, zp) \
	static_assert(offsetof(struct page, pg) == offsetof(struct zpdesc, zp))

ZPDESC_MATCH(flags, flags);
ZPDESC_MATCH(lru, lru);
ZPDESC_MATCH(mapping, mops);
ZPDESC_MATCH(index, next);
ZPDESC_MATCH(index, handle);
ZPDESC_MATCH(private, zspage);
ZPDESC_MATCH(page_type, first_obj_offset);
ZPDESC_MATCH(_refcount, _refcount);
#ifdef CONFIG_MEMCG
ZPDESC_MATCH(memcg_data, memcg_data);
#endif
#undef ZPDESC_MATCH
static_assert(sizeof(struct zpdesc) <= sizeof(struct page));

#define zpdesc_page(zp)			(_Generic((zp),			\
	const struct zpdesc *:		(const struct page *)(zp),	\
	struct zpdesc *:		(struct page *)(zp)))

#define zpdesc_folio(zp)		(_Generic((zp),			\
	const struct zpdesc *:		(const struct folio *)(zp),	\
	struct zpdesc *:		(struct folio *)(zp)))

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

static inline unsigned long zpdesc_pfn(struct zpdesc *zpdesc)
{
	return page_to_pfn(zpdesc_page(zpdesc));
}

static inline struct zpdesc *pfn_zpdesc(unsigned long pfn)
{
	return page_zpdesc(pfn_to_page(pfn));
}

static inline void __zpdesc_set_movable(struct zpdesc *zpdesc,
					const struct movable_operations *mops)
{
	__SetPageMovable(zpdesc_page(zpdesc), mops);
}

static inline void __zpdesc_clear_movable(struct zpdesc *zpdesc)
{
	__ClearPageMovable(zpdesc_page(zpdesc));
}

static inline void __zpdesc_clear_zsmalloc(struct zpdesc *zpdesc)
{
	__ClearPageZsmalloc(zpdesc_page(zpdesc));
}

static inline bool zpdesc_is_isolated(struct zpdesc *zpdesc)
{
	return PageIsolated(zpdesc_page(zpdesc));
}

static inline struct zone *zpdesc_zone(struct zpdesc *zpdesc)
{
	return page_zone(zpdesc_page(zpdesc));
}

static inline bool zpdesc_is_locked(struct zpdesc *zpdesc)
{
	return PageLocked(zpdesc_page(zpdesc));
}
#endif
