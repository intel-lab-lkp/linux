/* SPDX-License-Identifier: GPL-2.0 */
/* zpdesc.h: zswap.zpool memory descriptor
 *
 * Written by Alex Shi (Tencent) <alexs@kernel.org>
 */
#ifndef __MM_ZPDESC_H__
#define __MM_ZPDESC_H__

/*
 * struct zpdesc -	Memory descriptor for z3fold memory
 * @flags:		Page flags, PG_locked for headless z3fold memory
 * @lru:		Indirected used by page migration
 * @zppage_flag:	z3fold memory flags
 *
 * This struct overlays struct page for now. Do not modify without a good
 * understanding of the issues.
 */
struct zpdesc {
	unsigned long flags;
	struct list_head lru;
	unsigned long _zp_pad_1;
	unsigned long _zp_pad_2;
	unsigned long zppage_flag;
};
#define ZPDESC_MATCH(pg, zp) \
	static_assert(offsetof(struct page, pg) == offsetof(struct zpdesc, zp))

ZPDESC_MATCH(flags, flags);
ZPDESC_MATCH(lru, lru);
ZPDESC_MATCH(private, zppage_flag);
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

static inline void *zpdesc_address(const struct zpdesc *zpdesc)
{
	return folio_address(zpdesc_folio(zpdesc));
}

#endif
