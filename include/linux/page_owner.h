/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGE_OWNER_H
#define __LINUX_PAGE_OWNER_H

#include <linux/jump_label.h>

typedef int (folio_alloc_post_page_owner_t)(struct folio *folio, struct task_struct *tsk,
			void *data, char *kbuf, size_t count);

#ifdef CONFIG_PAGE_OWNER
extern struct static_key_false page_owner_inited;
extern struct page_ext_operations page_owner_ops;

extern void __reset_page_owner(struct page *page, unsigned short order);
extern void __set_page_owner(struct page *page,
			unsigned short order, gfp_t gfp_mask);
extern void __split_page_owner(struct page *page, unsigned int nr);
extern void __folio_copy_owner(struct folio *newfolio, struct folio *old);
extern void __set_page_owner_migrate_reason(struct page *page, int reason);
extern void __dump_page_owner(const struct page *page);
extern void pagetypeinfo_showmixedcount_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone);
extern void set_folio_alloc_post_page_owner(struct folio *folio,
		folio_alloc_post_page_owner_t *folio_alloc_post_page_owner, void *data);

static inline void reset_page_owner(struct page *page, unsigned short order)
{
	if (static_branch_unlikely(&page_owner_inited))
		__reset_page_owner(page, order);
}

static inline void set_page_owner(struct page *page,
			unsigned short order, gfp_t gfp_mask)
{
	if (static_branch_unlikely(&page_owner_inited))
		__set_page_owner(page, order, gfp_mask);
}

static inline void split_page_owner(struct page *page, unsigned int nr)
{
	if (static_branch_unlikely(&page_owner_inited))
		__split_page_owner(page, nr);
}
static inline void folio_copy_owner(struct folio *newfolio, struct folio *old)
{
	if (static_branch_unlikely(&page_owner_inited))
		__folio_copy_owner(newfolio, old);
}
static inline void set_page_owner_migrate_reason(struct page *page, int reason)
{
	if (static_branch_unlikely(&page_owner_inited))
		__set_page_owner_migrate_reason(page, reason);
}
static inline void dump_page_owner(const struct page *page)
{
	if (static_branch_unlikely(&page_owner_inited))
		__dump_page_owner(page);
}
#else
static inline void reset_page_owner(struct page *page, unsigned short order)
{
}
static inline void set_page_owner(struct page *page,
			unsigned int order, gfp_t gfp_mask)
{
}
static inline void split_page_owner(struct page *page,
			unsigned short order)
{
}
static inline void folio_copy_owner(struct folio *newfolio, struct folio *folio)
{
}
static inline void set_page_owner_migrate_reason(struct page *page, int reason)
{
}
static inline void dump_page_owner(const struct page *page)
{
}
static inline void set_folio_alloc_post_page_owner(struct folio *folio,
		folio_alloc_post_page_owner_t *folio_alloc_post_page_owner, void *data)
{
}
#endif /* CONFIG_PAGE_OWNER */
#endif /* __LINUX_PAGE_OWNER_H */
