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
	unsigned long _zp_pad_1;
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

#endif
