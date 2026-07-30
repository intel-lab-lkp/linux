// SPDX-License-Identifier: GPL-2.0
/*
 * C2C Function Browser - function-level cacheline sharing analysis
 *
 * Displays a 3-level hierarchy showing which functions share cachelines:
 *   Level 1: Read-side functions sorted by Cycles % (estimated load cycles)
 *   Level 2: Functions sampled writing the shared lines read by level 1
 *   Level 3: The specific cachelines where the two functions contend
 *
 * Builds the hierarchy from the existing cacheline histograms
 * (c2c_hist_entry->hists), reusing the shared c2c data structures.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <tools/libc_compat.h> /* reallocarray */
#include <asm/bug.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/zalloc.h>

#include "../browser.h"
#include "../keysyms.h"
#include "../libslang.h"
#include "../ui.h"
#include "../../util/addr_location.h"
#include "../../util/cacheline.h"
#include "../../util/debug.h"
#include "../../util/hist.h"
#include "../../util/map.h"
#include "../../util/mem-events.h"
#include "../../util/mem-info.h"
#include "../../util/sort.h"
#include "../../util/symbol.h"
#include "../../util/thread.h"
#include "../../c2c.h"
#include "hists.h"

struct perf_c2c_ext {
	struct c2c_hists	function_hists;
	/* Total estimated cycles across all level-1 entries. */
	u64			total_cycles;
};

static struct perf_c2c_ext c2c_ext __maybe_unused;

struct c2c_function_browser {
	struct hist_browser	hb;
};

static inline __maybe_unused u64 c2c_hitm_count(const struct c2c_stats *stats)
{
	return stats->tot_hitm;
}

static inline __maybe_unused bool symbol_name_equal(struct symbol *a, struct symbol *b)
{
	/* Two unknown symbols compare equal, matching cmp_null() in util/sort.c. */
	if (!a || !b)
		return a == b;
	return arch__compare_symbol_names(a->name, b->name) == 0;
}

static inline __maybe_unused u64 hist_entry__iaddr(struct hist_entry *he)
{
	if (he->mem_info)
		return mem_info__iaddr(he->mem_info)->addr;
	return he->ip;
}

int perf_c2c__browse_function_view(void)
{
	ui__warning("C2C function view is not implemented yet.\n");
	return 0;
}
