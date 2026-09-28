/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_CRASH_MEMACTION_H
#define LINUX_CRASH_MEMACTION_H

#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/types.h>

struct kimage;

enum crash_memaction_types {
	CRASH_MEMACTION_CACHE = 0x1000,
	CRASH_MEMACTION_SECRET = 0x2000,
};

#define CRASH_MEMACTION_NOTE_NAME	"MEMACTION"

/*
 * Bit n of the bitmap at @bitmap_paddr stands for the page frame at
 * @start_pfn + n.
 */
struct crash_memaction_region_note {
	u64 start_pfn;
	u64 nr_pages;
	u64 bitmap_paddr;
};

/* Wire format between the two kernels */
struct crash_memaction_note {
	u32 types; /* CRASH_MEMACTION_* types that set bitmap bits stand for */
	u32 page_shift;
	u32 nr_regions;
	u32 _pad;
	struct crash_memaction_region_note regions[];
};

#ifdef CONFIG_CRASH_MEMACTION
void __init crash_memaction_init(void);

DECLARE_STATIC_KEY_FALSE(crash_memaction_active);

/* Safe from any context. */
void __crash_memaction_mark_pfns(unsigned long pfn, unsigned long nr_pages);
void __crash_memaction_unmark_pfns(unsigned long pfn, unsigned long nr_pages);

static inline void crash_memaction_unmark_pfns(unsigned long pfn,
		unsigned long nr_pages)
{
	if (static_branch_unlikely(&crash_memaction_active))
		__crash_memaction_unmark_pfns(pfn, nr_pages);
}

void crash_memaction_mark(void *addr, size_t size, int types);
void crash_memaction_unmark(void *addr, size_t size);

int crash_memaction_types(void);
ssize_t crash_memaction_types_str(char *buf);

int crash_load_memaction(struct kimage *image);
void crash_memaction_unload(struct kimage *image);
#else
static inline void crash_memaction_init(void) { }
static inline void crash_memaction_unmark_pfns(unsigned long pfn,
		unsigned long nr_pages) { }
static inline void crash_memaction_mark(void *addr, size_t size, int types) { }
static inline void crash_memaction_unmark(void *addr, size_t size) { }
static inline int crash_memaction_types(void) { return 0; }
static inline int crash_load_memaction(struct kimage *image) { return 0; }
static inline void crash_memaction_unload(struct kimage *image) { }
#endif /* CONFIG_CRASH_MEMACTION */
#endif /* LINUX_CRASH_MEMACTION_H */
