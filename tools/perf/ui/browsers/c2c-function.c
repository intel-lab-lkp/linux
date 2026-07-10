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

/*
 * Sum of the level-2 children's store counts under a level-1 hist_entry.
 * Read from the cache populated by the hierarchy builder, so this is O(1)
 * and safe to call from the sort comparator.
 */
static __maybe_unused u64 hist_entry__child_stores(struct hist_entry *he)
{
	struct c2c_hist_entry *c2c_he = container_of(he, struct c2c_hist_entry, he);

	return c2c_he->child_stores;
}

static __maybe_unused int
total_stores_entry(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
		   struct hist_entry *he)
{
	struct c2c_hist_entry *c2c_he = container_of(he, struct c2c_hist_entry, he);
	int width = c2c_width(fmt, hpp, he->hists);
	u64 total;

	/* L1 shows the sum of sharing-function stores; L2/L3 show their own. */
	total = he->parent_he ? (u64)c2c_he->stats.store : hist_entry__child_stores(he);

	return scnprintf(hpp->buf, hpp->size, "%*" PRIu64, width, total);
}

/*
 * cacheline_symbol_entry - Render cacheline address for function view
 */
static __maybe_unused int
cacheline_symbol_entry(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
		       struct hist_entry *he)
{
	int width = c2c_width(fmt, hpp, he->hists);
	char buf[24];
	u64 addr;

	/* Only show the address on level-3 cacheline entries. */
	if (!he->parent_he || !he->parent_he->parent_he || !he->mem_info)
		return scnprintf(hpp->buf, hpp->size, "%*s", width, "");

	addr = cl_address(mem_info__daddr(he->mem_info)->addr, chk_double_cl);
	scnprintf(buf, sizeof(buf), "0x%" PRIx64, addr);

	return scnprintf(hpp->buf, hpp->size, "%*s", width, buf);
}

/* Render the code (instruction) address for level-1 and level-2 entries. */
static __maybe_unused int
iaddr_symbol_entry(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
		   struct hist_entry *he)
{
	int width = c2c_width(fmt, hpp, he->hists);
	int iaddr_width, ret;
	char buf[24];
	u64 addr;
	char folded_sign;

	/* Hide for cacheline (level-3) entries. */
	if (he->parent_he && he->parent_he->parent_he)
		return scnprintf(hpp->buf, hpp->size, "%*s", width, "");

	addr = hist_entry__iaddr(he);

	folded_sign = he->has_children ? (he->unfolded ? '-' : '+') : ' ';
	ret = scnprintf(hpp->buf, hpp->size, "%c ", folded_sign);

	iaddr_width = width - ret;
	if (iaddr_width <= 0)
		return ret;

	scnprintf(buf, sizeof(buf), "0x%" PRIx64, addr);
	ret += scnprintf(hpp->buf + ret, hpp->size - ret, "%*.*s", iaddr_width, iaddr_width, buf);
	return ret;
}

/*
 * symbol_view_entry - Render symbol name for function view with expansion indicators
 */
static __maybe_unused int
symbol_view_entry(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
		  struct hist_entry *he)
{
	int width = c2c_width(fmt, hpp, he->hists);
	int sym_width;
	int ret;
	char symbuf[512];
	char folded_sign;

	/* Hide Symbol for cacheline entries */
	if (he->parent_he && he->parent_he->parent_he)
		return scnprintf(hpp->buf, hpp->size, "%*s", width, "");

	folded_sign = he->has_children ? (he->unfolded ? '-' : '+') : ' ';

	ret = scnprintf(hpp->buf, hpp->size, "%c ", folded_sign);

	sym_width = width - ret;

	if (sym_width <= 0)
		return ret;

	/* sort_sym.se_snprintf is statically set and never cleared. */
	sort_sym.se_snprintf(he, symbuf, sizeof(symbuf), sym_width);

	ret += scnprintf(hpp->buf + ret, hpp->size - ret, "%-*.*s", sym_width, sym_width, symbuf);
	return ret;
}

/*
 * cycles_percent_entry - Render cycles percentage column
 */
static __maybe_unused int
cycles_percent_entry(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
		     struct hist_entry *he)
{
	struct c2c_hist_entry *c2c_he;
	int width = c2c_width(fmt, hpp, he->hists);
	u64 fn_cycles, total_cycles;
	char folded_sign;
	double pct;
	int ret, pct_width;

	/* Hide Cycles Percent for child functions and cachelines. */
	if (he->parent_he)
		return scnprintf(hpp->buf, hpp->size, "%*s", width, "");

	c2c_he = container_of(he, struct c2c_hist_entry, he);
	fn_cycles = c2c_hist_entry__cycles(c2c_he);
	/* Populated by build_function_view_hierarchy() once the L1 tree is built. */
	total_cycles = c2c_ext.total_cycles;
	pct = total_cycles > 0 ? (double)fn_cycles / total_cycles * 100.0 : 0.0;

	/* Add folded sign only for level-1 entries */
	folded_sign = he->has_children ? (he->unfolded ? '-' : '+') : ' ';
	ret = scnprintf(hpp->buf, hpp->size, "%c ", folded_sign);

	pct_width = width - ret;
	if (pct_width <= 0)
		return ret;
	ret += scnprintf(hpp->buf + ret, hpp->size - ret, "%*.2f%%", pct_width - 1, pct);
	return ret;
}

/*
 * cycles_percent_cmp - Comparison function for cycles percentage sorting
 */
static __maybe_unused int64_t
cycles_percent_cmp(struct perf_hpp_fmt *fmt __maybe_unused,
		   struct hist_entry *left, struct hist_entry *right)
{
	struct c2c_hist_entry *c2c_left = container_of(left, struct c2c_hist_entry, he);
	struct c2c_hist_entry *c2c_right = container_of(right, struct c2c_hist_entry, he);
	u64 cycles_left, cycles_right;

	/* Cycles Percent is only shown for level-1 entries; others compare equal. */
	if (left->parent_he || right->parent_he)
		return 0;

	cycles_left = c2c_hist_entry__cycles(c2c_left);
	cycles_right = c2c_hist_entry__cycles(c2c_right);

	return (cycles_left > cycles_right) - (cycles_left < cycles_right);
}

/*
 * iaddr_symbol_cmp - Comparison function for instruction address sorting
 */
static __maybe_unused int64_t
iaddr_symbol_cmp(struct perf_hpp_fmt *fmt __maybe_unused,
		 struct hist_entry *left, struct hist_entry *right)
{
	u64 left_iaddr, right_iaddr;

	/* IAddr is hidden for level-3 cacheline entries; they compare equal. */
	if ((left->parent_he && left->parent_he->parent_he) ||
	    (right->parent_he && right->parent_he->parent_he))
		return 0;

	left_iaddr = hist_entry__iaddr(left);
	right_iaddr = hist_entry__iaddr(right);

	/*
	 * Order by instruction address, same direction as sort__iaddr_cmp()
	 * (which returns r - l). Uses hist_entry__iaddr(), which falls back to
	 * he->ip when mem_info is NULL, so it matches what iaddr_symbol_entry()
	 * displays.
	 */
	return (left_iaddr < right_iaddr) - (left_iaddr > right_iaddr);
}

static __maybe_unused int64_t
empty_cmp(struct perf_hpp_fmt *fmt __maybe_unused,
	  struct hist_entry *left __maybe_unused,
	  struct hist_entry *right __maybe_unused)
{
	return 0;
}

/*
 * total_stores_cmp - Comparison function for total stores sorting
 */
static __maybe_unused int64_t
total_stores_cmp(struct perf_hpp_fmt *fmt __maybe_unused,
		 struct hist_entry *left, struct hist_entry *right)
{
	struct c2c_hist_entry *c2c_left = container_of(left, struct c2c_hist_entry, he);
	struct c2c_hist_entry *c2c_right = container_of(right, struct c2c_hist_entry, he);
	u64 left_store, right_store;

	/* Match total_stores_entry(): L1 sums child stores, L2/L3 use their own. */
	left_store = left->parent_he ? (u64)c2c_left->stats.store :
				       hist_entry__child_stores(left);
	right_store = right->parent_he ? (u64)c2c_right->stats.store :
					 hist_entry__child_stores(right);

	return (left_store > right_store) - (left_store < right_store);
}

int perf_c2c__browse_function_view(struct hists *hists __maybe_unused)
{
	ui__warning("C2C function view is not implemented yet.\n");
	return -ENOSYS;
}
