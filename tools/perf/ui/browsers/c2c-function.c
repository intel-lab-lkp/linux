// SPDX-License-Identifier: GPL-2.0
/*
 * C2C Function Browser - function-level cacheline sharing analysis
 *
 * Planned UI: 3-level hierarchy showing which functions share cachelines (not implemented yet):
 *   Level 1: Primary functions sorted by Cycles % (estimated load cycles)
 *   Level 2: Other functions sharing cachelines with the level-1 function
 *   Level 3: Specific shared cachelines between each pair of functions
 *
 * Uses c2c_hist_entry->hists to build the hierarchy without adding any
 * per-entry state to the existing c2c data structures.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/zalloc.h>

#include "../browser.h"
#include "../helpline.h"
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
	/* Cached across all level-1 entries; 0 means "not yet computed". */
	u64			total_cycles;
};

static struct perf_c2c_ext c2c_ext __maybe_unused;

struct c2c_function_browser {
	struct hist_browser	hb;
};

static __maybe_unused inline u64 c2c_hitm_count(const struct c2c_stats *stats)
{
	return stats->tot_hitm;
}

static __maybe_unused inline bool symbol_name_equal(struct symbol *a, struct symbol *b)
{
	return a && b && arch__compare_symbol_names(a->name, b->name) == 0;
}

static __maybe_unused inline u64 hist_entry__iaddr(struct hist_entry *he)
{
	if (he->mem_info)
		return mem_info__iaddr(he->mem_info)->addr;
	return he->ip;
}

static __maybe_unused int symbol_width(struct hists *hists, struct sort_entry *se)
{
	int width = hists__col_len(hists, se->se_width_idx);

	if (!c2c.symbol_full && width > SYMBOL_WIDTH)
		width = SYMBOL_WIDTH;

	return width;
}

static struct c2c_dimension dim_symbol_view;

/*
 * c2c_width - Calculate width for a C2C column in function view
 */
static __maybe_unused int c2c_width(struct perf_hpp_fmt *fmt,
		     struct perf_hpp *hpp __maybe_unused,
		     struct hists *hists)
{
	struct c2c_fmt *c2c_fmt;
	struct c2c_dimension *dim;

	c2c_fmt = container_of(fmt, struct c2c_fmt, fmt);
	dim = c2c_fmt->dim;

	if (dim == &dim_symbol_view)
		return symbol_width(hists, dim->se);

	return dim->se ? hists__col_len(hists, dim->se->se_width_idx) :
			 dim->width;
}

static __maybe_unused int c2c_header(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
		      struct hists *hists, int line, int *span)
{
	struct c2c_fmt *c2c_fmt;
	struct c2c_dimension *dim;
	const char *text = NULL;
	int width = c2c_width(fmt, hpp, hists);

	c2c_fmt = container_of(fmt, struct c2c_fmt, fmt);
	dim = c2c_fmt->dim;

	if (dim->se) {
		text = dim->header.line[line].text;
		/* Use the last line from sort_entry if not defined. */
		if (!text && line == hists->hpp_list->nr_header_lines - 1)
			text = dim->se->se_header;
	} else {
		text = dim->header.line[line].text;

		if (span) {
			if (*span) {
				(*span)--;
				return 0;
			}

			*span = dim->header.line[line].span;
		}
	}

	if (text == NULL)
		text = "";

	return scnprintf(hpp->buf, hpp->size, "%*s", width, text);
}

/*
 * Return the estimated total cycles for a c2c_hist_entry
 * (rmt_hitm + lcl_hitm + rmt_peer + lcl_peer + other loads).
 */
static __maybe_unused u64 c2c_hist_entry__cycles(struct c2c_hist_entry *c2c_he)
{
	double cycles_rmt, cycles_lcl, cycles_load;
	u64 other_load, total_hitm;

	cycles_rmt = avg_stats(&c2c_he->cstats.rmt_hitm) * c2c_he->stats.rmt_hitm;
	cycles_lcl = avg_stats(&c2c_he->cstats.lcl_hitm) * c2c_he->stats.lcl_hitm;
	total_hitm = c2c_he->stats.tot_hitm;
	other_load = (c2c_he->stats.load >= total_hitm) ? c2c_he->stats.load - total_hitm : 0;
	cycles_load = avg_stats(&c2c_he->cstats.load) * other_load;

	return (u64)(cycles_rmt + cycles_lcl + cycles_load);
}

/* Sum c2c_hist_entry__cycles() across all level-1 entries. */
static __maybe_unused u64 c2c_ext__total_cycles(void)
{
	struct rb_node *nd;
	u64 total = 0;

	for (nd = rb_first_cached(&c2c_ext.function_hists.hists.entries); nd;
	     nd = rb_next(nd)) {
		struct hist_entry *he = rb_entry(nd, struct hist_entry, rb_node);
		struct c2c_hist_entry *c2c_he = container_of(he, struct c2c_hist_entry, he);

		total += c2c_hist_entry__cycles(c2c_he);
	}
	return total;
}

/* Sum child entries' store counts under a level-1 hist_entry. */
static __maybe_unused u64 hist_entry__child_stores(struct hist_entry *he)
{
	struct rb_node *nd;
	u64 sum = 0;

	for (nd = rb_first_cached(&he->hroot_out); nd; nd = rb_next(nd)) {
		struct hist_entry *child = rb_entry(nd, struct hist_entry, rb_node);
		struct c2c_hist_entry *c2c_child =
			container_of(child, struct c2c_hist_entry, he);

		sum += (u64)c2c_child->stats.store;
	}
	return sum;
}

int perf_c2c__browse_function_view(struct hists *hists __maybe_unused)
{
	ui__warning("C2C function view is not implemented yet.\n");
	return -ENOSYS;
}
