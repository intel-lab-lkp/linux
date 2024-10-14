/* SPDX-License-Identifier: GPL-2.0 */
/*
 * page allocation tagging
 */
#ifndef _LINUX_PGALLOC_TAG_H
#define _LINUX_PGALLOC_TAG_H

#include <linux/alloc_tag.h>

#ifdef CONFIG_MEM_ALLOC_PROFILING

#ifdef CONFIG_PGALLOC_TAG_USE_PAGEFLAGS

typedef u16	pgalloc_tag_ref;

extern struct alloc_tag_kernel_section kernel_tags;

#define CODETAG_ID_NULL		0
#define CODETAG_ID_EMPTY	1
#define CODETAG_ID_FIRST	2

#ifdef CONFIG_MODULES

extern struct alloc_tag_module_section module_tags;

static inline struct codetag *get_module_ct(pgalloc_tag_ref pgref)
{
	return &module_tags.first_tag[pgref - kernel_tags.count].ct;
}

static inline pgalloc_tag_ref get_module_pgref(struct alloc_tag *tag)
{
	return CODETAG_ID_FIRST + kernel_tags.count + (tag - module_tags.first_tag);
}

#else /* CONFIG_MODULES */

static inline struct codetag *get_module_ct(pgalloc_tag_ref pgref)
{
	pr_warn("invalid page tag reference %lu\n", (unsigned long)pgref);
	return NULL;
}

static inline pgalloc_tag_ref get_module_pgref(struct alloc_tag *tag)
{
	pr_warn("invalid page tag 0x%lx\n", (unsigned long)tag);
	return CODETAG_ID_NULL;
}

#endif /* CONFIG_MODULES */

static inline void read_pgref(pgalloc_tag_ref *pgref, union codetag_ref *ref)
{
	pgalloc_tag_ref pgref_val = *pgref;

	switch (pgref_val) {
	case (CODETAG_ID_NULL):
		ref->ct = NULL;
		break;
	case (CODETAG_ID_EMPTY):
		set_codetag_empty(ref);
		break;
	default:
		pgref_val -= CODETAG_ID_FIRST;
		ref->ct = pgref_val < kernel_tags.count ?
			&kernel_tags.first_tag[pgref_val].ct :
			get_module_ct(pgref_val);
		break;
	}
}

static inline void write_pgref(pgalloc_tag_ref *pgref, union codetag_ref *ref)
{
	struct alloc_tag *tag;

	if (!ref->ct) {
		*pgref = CODETAG_ID_NULL;
		return;
	}

	if (is_codetag_empty(ref)) {
		*pgref = CODETAG_ID_EMPTY;
		return;
	}

	tag = ct_to_alloc_tag(ref->ct);
	if (tag >= kernel_tags.first_tag && tag < kernel_tags.first_tag + kernel_tags.count) {
		*pgref = CODETAG_ID_FIRST + (tag - kernel_tags.first_tag);
		return;
	}

	*pgref = get_module_pgref(tag);
}

void __init alloc_tag_sec_init(void);

#else /* CONFIG_PGALLOC_TAG_USE_PAGEFLAGS */

typedef union codetag_ref	pgalloc_tag_ref;

static inline void read_pgref(pgalloc_tag_ref *pgref, union codetag_ref *ref)
{
	ref->ct = pgref->ct;
}

static inline void write_pgref(pgalloc_tag_ref *pgref, union codetag_ref *ref)
{
	pgref->ct = ref->ct;
}

static inline void alloc_tag_sec_init(void) {}

#endif /* CONFIG_PGALLOC_TAG_USE_PAGEFLAGS */

union pgtag_ref_handle {
	pgalloc_tag_ref *pgref;	/* reference in page extension */
	struct page *page;	/* reference in page flags */
};

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

/* Should be called only if mem_alloc_profiling_enabled() */
static inline bool page_ext_get_page_tag_ref(struct page *page, union codetag_ref *ref,
					     union pgtag_ref_handle *handle)
{
	struct page_ext *page_ext;
	pgalloc_tag_ref *pgref;

	if (!page)
		return false;

	page_ext = page_ext_get(page);
	if (!page_ext)
		return false;

	pgref = pgref_from_page_ext(page_ext);
	read_pgref(pgref, ref);
	handle->pgref = pgref;
	return true;
}

static inline void page_ext_put_page_tag_ref(union pgtag_ref_handle handle)
{
	if (WARN_ON(!handle.pgref))
		return;

	page_ext_put(page_ext_from_pgref(handle.pgref));
}

static inline void page_ext_update_page_tag_ref(union pgtag_ref_handle handle,
						union codetag_ref *ref)
{
	if (WARN_ON(!handle.pgref || !ref))
		return;

	write_pgref(handle.pgref, ref);
}

#ifdef CONFIG_PGALLOC_TAG_USE_PAGEFLAGS

DECLARE_STATIC_KEY_TRUE(mem_profiling_use_pageflags);
extern unsigned long alloc_tag_ref_mask;
extern int alloc_tag_ref_offs;

/* Should be called only if mem_alloc_profiling_enabled() */
static inline bool get_page_tag_ref(struct page *page, union codetag_ref *ref,
				    union pgtag_ref_handle *handle)
{
	pgalloc_tag_ref pgref;

	if (!static_key_enabled(&mem_profiling_use_pageflags))
		return page_ext_get_page_tag_ref(page, ref, handle);

	if (!page)
		return false;

	pgref = (page->flags >> alloc_tag_ref_offs) & alloc_tag_ref_mask;
	read_pgref(&pgref, ref);
	handle->page = page;
	return true;
}

static inline void put_page_tag_ref(union pgtag_ref_handle handle)
{
	if (!static_key_enabled(&mem_profiling_use_pageflags)) {
		page_ext_put_page_tag_ref(handle);
		return;
	}

	WARN_ON(!handle.page);
}

static inline void update_page_tag_ref(union pgtag_ref_handle handle, union codetag_ref *ref)
{
	unsigned long old_flags, flags, val;
	struct page *page = handle.page;
	pgalloc_tag_ref pgref;

	if (!static_key_enabled(&mem_profiling_use_pageflags)) {
		page_ext_update_page_tag_ref(handle, ref);
		return;
	}

	if (WARN_ON(!page || !ref))
		return;

	write_pgref(&pgref, ref);
	val = (unsigned long)pgref;
	val = (val & alloc_tag_ref_mask) << alloc_tag_ref_offs;
	do {
		old_flags = READ_ONCE(page->flags);
		flags = old_flags;
		flags &= ~(alloc_tag_ref_mask << alloc_tag_ref_offs);
		flags |= val;
	} while (unlikely(!try_cmpxchg(&page->flags, &old_flags, flags)));
}

#else /* CONFIG_PGALLOC_TAG_USE_PAGEFLAGS */

/* Should be called only if mem_alloc_profiling_enabled() */
static inline bool get_page_tag_ref(struct page *page, union codetag_ref *ref,
				    union pgtag_ref_handle *handle)
{
	return page_ext_get_page_tag_ref(page, ref, handle);
}

static inline void put_page_tag_ref(union pgtag_ref_handle handle)
{
	page_ext_put_page_tag_ref(handle);
}

static inline void update_page_tag_ref(union pgtag_ref_handle handle,
				       union codetag_ref *ref)
{
	page_ext_update_page_tag_ref(handle, ref);
}

#endif /* CONFIG_PGALLOC_TAG_USE_PAGEFLAGS */

static inline void clear_page_tag_ref(struct page *page)
{
	if (mem_alloc_profiling_enabled()) {
		union pgtag_ref_handle handle;
		union codetag_ref ref;

		if (get_page_tag_ref(page, &ref, &handle)) {
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
		union pgtag_ref_handle handle;
		union codetag_ref ref;

		if (get_page_tag_ref(page, &ref, &handle)) {
			alloc_tag_add(&ref, task->alloc_tag, PAGE_SIZE * nr);
			update_page_tag_ref(handle, &ref);
			put_page_tag_ref(handle);
		}
	}
}

static inline void pgalloc_tag_sub(struct page *page, unsigned int nr)
{
	if (mem_alloc_profiling_enabled()) {
		union pgtag_ref_handle handle;
		union codetag_ref ref;

		if (get_page_tag_ref(page, &ref, &handle)) {
			alloc_tag_sub(&ref, PAGE_SIZE * nr);
			update_page_tag_ref(handle, &ref);
			put_page_tag_ref(handle);
		}
	}
}

static inline struct alloc_tag *pgalloc_tag_get(struct page *page)
{
	struct alloc_tag *tag = NULL;

	if (mem_alloc_profiling_enabled()) {
		union pgtag_ref_handle handle;
		union codetag_ref ref;

		if (get_page_tag_ref(page, &ref, &handle)) {
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

static inline void clear_page_tag_ref(struct page *page) {}
static inline void pgalloc_tag_add(struct page *page, struct task_struct *task,
				   unsigned int nr) {}
static inline void pgalloc_tag_sub(struct page *page, unsigned int nr) {}
static inline struct alloc_tag *pgalloc_tag_get(struct page *page) { return NULL; }
static inline void pgalloc_tag_sub_pages(struct alloc_tag *tag, unsigned int nr) {}
static inline void alloc_tag_sec_init(void) {}

#endif /* CONFIG_MEM_ALLOC_PROFILING */

#endif /* _LINUX_PGALLOC_TAG_H */
