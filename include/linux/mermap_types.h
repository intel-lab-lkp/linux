/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MERMAP_TYPES_H
#define _LINUX_MERMAP_TYPES_H

#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/types.h>

#ifdef CONFIG_MERMAP

/* Tracks an individual allocation in the mermap. */
struct mermap_alloc {
	/* Currently allocated. */
	bool in_use;
	/* Requires flush before reallocating. */
	bool need_flush;
	unsigned long base;
	/* Non-inclusive. */
	unsigned long end;
};

struct mermap_cpu {
	/* Next address immediately available for alloc (no TLB flush needed). */
	unsigned long next_addr;
	struct mermap_alloc allocs[4];
#if IS_ENABLED(CONFIG_MERMAP_KUNIT_TEST)
	u64 tlb_flushes;
#endif
};

struct mermap {
	struct mutex init_lock;
	struct mermap_cpu __percpu *cpu;
};

#else /* CONFIG_MERMAP */

struct mermap {};

#endif /* CONFIG_MERMAP */

#endif /* _LINUX_MERMAP_TYPES_H */

