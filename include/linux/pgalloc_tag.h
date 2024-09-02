/* SPDX-License-Identifier: GPL-2.0 */
/*
 * page allocation tagging
 */
#ifndef _LINUX_PGALLOC_TAG_H
#define _LINUX_PGALLOC_TAG_H

#include <linux/alloc_tag.h>

#ifdef CONFIG_MEM_ALLOC_PROFILING

typedef union codetag_ref	pgalloc_tag_ref;

static inline void read_pgref(pgalloc_tag_ref *pgref, union codetag_ref *ref)
{
	ref->ct = pgref->ct;
}

static inline void write_pgref(pgalloc_tag_ref *pgref, union codetag_ref *ref)
{
	pgref->ct = ref->ct;
}
#include <linux/page_ext.h>

extern struct page_ext_operations page_alloc_tagging_ops;

static inline pgalloc_tag_ref *pgref_from_page_ext(struct page_ext *page_ext)
{
	return (pgalloc_tag_ref *)page_ext_data(page_ext, &page_alloc_tagging_ops);
}

static inline struct page_ext *page_ext_from_pgref(pgalloc_tag_ref *pgref)
{
	return (void *)pgref - page_alloc_tagging_ops.offset;
}

typedef pgalloc_tag_ref	*pgtag_ref_handle;

/* Should be called only if mem_alloc_profiling_enabled() */
static inline pgtag_ref_handle get_page_tag_ref(struct page *page, union codetag_ref *ref)
{
	if (page) {
		struct page_ext *page_ext = page_ext_get(page);

		if (page_ext) {
			pgalloc_tag_ref *pgref = pgref_from_page_ext(page_ext);

			read_pgref(pgref, ref);
			return pgref;
		}
	}
	return NULL;
}

static inline void put_page_tag_ref(pgtag_ref_handle pgref)
{
	if (WARN_ON(!pgref))
		return;

	page_ext_put(page_ext_from_pgref(pgref));
}

static inline void update_page_tag_ref(pgtag_ref_handle pgref, union codetag_ref *ref)
{
	if (WARN_ON(!pgref || !ref))
		return;

	write_pgref(pgref, ref);
}

static inline void clear_page_tag_ref(struct page *page)
{
	if (mem_alloc_profiling_enabled()) {
		pgtag_ref_handle handle;
		union codetag_ref ref;

		handle = get_page_tag_ref(page, &ref);
		if (handle) {
			set_codetag_empty(&ref);
			update_page_tag_ref(handle, &ref);
			put_page_tag_ref(handle);
		}
	}
}

static inline void pgalloc_tag_add(struct page *page, struct task_struct *task,
				   unsigned int nr)
{
	if (mem_alloc_profiling_enabled()) {
		pgtag_ref_handle handle;
		union codetag_ref ref;

		handle = get_page_tag_ref(page, &ref);
		if (handle) {
			alloc_tag_add(&ref, task->alloc_tag, PAGE_SIZE * nr);
			update_page_tag_ref(handle, &ref);
			put_page_tag_ref(handle);
		}
	}
}

static inline void pgalloc_tag_sub(struct page *page, unsigned int nr)
{
	if (mem_alloc_profiling_enabled()) {
		pgtag_ref_handle handle;
		union codetag_ref ref;

		handle = get_page_tag_ref(page, &ref);
		if (handle) {
			alloc_tag_sub(&ref, PAGE_SIZE * nr);
			update_page_tag_ref(handle, &ref);
			put_page_tag_ref(handle);
		}
	}
}

static inline void pgalloc_tag_split(struct page *page, unsigned int nr)
{
	pgtag_ref_handle first_pgref;
	union codetag_ref first_ref;
	struct alloc_tag *tag;
	int i;

	if (!mem_alloc_profiling_enabled())
		return;

	first_pgref = get_page_tag_ref(page, &first_ref);
	if (unlikely(!first_pgref))
		return;

	if (!first_ref.ct)
		goto out;

	tag = ct_to_alloc_tag(first_ref.ct);
	for (i = 1; i < nr; i++) {
		pgtag_ref_handle handle;
		union codetag_ref ref;

		page++;
		handle = get_page_tag_ref(page, &ref);
		if (handle) {
			/* Set new reference to point to the original tag */
			alloc_tag_add_check(&ref, tag);
			ref.ct = &tag->ct;
			/*
			 * We need in increment the call counter every time we split a
			 * large allocation into smaller ones because when we free each
			 * part the counter will be decremented.
			 */
			this_cpu_inc(tag->counters->calls);
			update_page_tag_ref(handle, &ref);
			put_page_tag_ref(handle);
		}
	}
out:
	put_page_tag_ref(first_pgref);
}

static inline struct alloc_tag *pgalloc_tag_get(struct page *page)
{
	struct alloc_tag *tag = NULL;

	if (mem_alloc_profiling_enabled()) {
		pgtag_ref_handle handle;
		union codetag_ref ref;

		handle = get_page_tag_ref(page, &ref);
		if (handle) {
			alloc_tag_sub_check(&ref);
			if (ref.ct)
				tag = ct_to_alloc_tag(ref.ct);
			put_page_tag_ref(handle);
		}
	}

	return tag;
}

static inline void pgalloc_tag_sub_pages(struct alloc_tag *tag, unsigned int nr)
{
	if (mem_alloc_profiling_enabled() && tag)
		this_cpu_sub(tag->counters->bytes, PAGE_SIZE * nr);
}

#else /* CONFIG_MEM_ALLOC_PROFILING */

typedef void	*pgtag_ref_handle;

static inline pgtag_ref_handle
get_page_tag_ref(struct page *page, union codetag_ref *ref) { return NULL; }
static inline void put_page_tag_ref(pgtag_ref_handle handle) {}
static inline void update_page_tag_ref(pgtag_ref_handle handle, union codetag_ref *ref) {}
static inline void clear_page_tag_ref(struct page *page) {}
static inline void pgalloc_tag_add(struct page *page, struct task_struct *task,
				   unsigned int nr) {}
static inline void pgalloc_tag_sub(struct page *page, unsigned int nr) {}
static inline void pgalloc_tag_split(struct page *page, unsigned int nr) {}
static inline struct alloc_tag *pgalloc_tag_get(struct page *page) { return NULL; }
static inline void pgalloc_tag_sub_pages(struct alloc_tag *tag, unsigned int nr) {}

#endif /* CONFIG_MEM_ALLOC_PROFILING */

#endif /* _LINUX_PGALLOC_TAG_H */
