// SPDX-License-Identifier: GPL-2.0
/*
 * C2C Function Browser - function-level cacheline sharing analysis
 *
 * Displays a 3-level hierarchy showing which functions share cachelines:
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
#include <tools/libc_compat.h> /* reallocarray */
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

static struct perf_c2c_ext c2c_ext;

struct c2c_function_browser {
	struct hist_browser	hb;
};

static inline u64 c2c_hitm_count(const struct c2c_stats *stats)
{
	return stats->tot_hitm;
}

static inline bool symbol_name_equal(struct symbol *a, struct symbol *b)
{
	return a && b && arch__compare_symbol_names(a->name, b->name) == 0;
}

static inline u64 hist_entry__iaddr(struct hist_entry *he)
{
	if (he->mem_info)
		return mem_info__iaddr(he->mem_info)->addr;
	return he->ip;
}

static int symbol_width(struct hists *hists, struct sort_entry *se)
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
static int c2c_width(struct perf_hpp_fmt *fmt,
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

static int c2c_header(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
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
static u64 c2c_hist_entry__cycles(struct c2c_hist_entry *c2c_he)
{
	double cycles_rmt, cycles_lcl, cycles_rmt_peer, cycles_lcl_peer, cycles_load;
	u64 categorized, other_load;

	/*
	 * compute_stats() in builtin-c2c.c assigns each load sample to exactly
	 * one cstats bucket (rmt_hitm, lcl_hitm, rmt_peer, lcl_peer or load),
	 * while stats.load counts every load. Weight each category by its own
	 * average and treat only the uncategorized remainder as plain loads, so
	 * peer-snoop cycles are neither dropped nor charged the load average.
	 */
	cycles_rmt = avg_stats(&c2c_he->cstats.rmt_hitm) * c2c_he->stats.rmt_hitm;
	cycles_lcl = avg_stats(&c2c_he->cstats.lcl_hitm) * c2c_he->stats.lcl_hitm;
	cycles_rmt_peer = avg_stats(&c2c_he->cstats.rmt_peer) * c2c_he->stats.rmt_peer;
	cycles_lcl_peer = avg_stats(&c2c_he->cstats.lcl_peer) * c2c_he->stats.lcl_peer;

	categorized = (u64)c2c_he->stats.tot_hitm + c2c_he->stats.tot_peer;
	other_load = (c2c_he->stats.load >= categorized) ?
		     c2c_he->stats.load - categorized : 0;
	cycles_load = avg_stats(&c2c_he->cstats.load) * other_load;

	return (u64)(cycles_rmt + cycles_lcl + cycles_rmt_peer +
		     cycles_lcl_peer + cycles_load);
}

/* Sum c2c_hist_entry__cycles() across all level-1 entries. */
static u64 c2c_ext__total_cycles(void)
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
static u64 hist_entry__child_stores(struct hist_entry *he)
{
	struct c2c_hist_entry *c2c_he = container_of(he, struct c2c_hist_entry, he);

	return c2c_he->child_stores;
}

static int
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
static int
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
static int
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
static int
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
static int
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
static int64_t
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
static int64_t
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

static int64_t
empty_cmp(struct perf_hpp_fmt *fmt __maybe_unused,
	  struct hist_entry *left __maybe_unused,
	  struct hist_entry *right __maybe_unused)
{
	return 0;
}

/*
 * total_stores_cmp - Comparison function for total stores sorting
 */
static int64_t
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

/*
 * Function view dimensions
 */
static struct c2c_dimension dim_cycles_percent = {
	.header		= HEADER_BOTH("Cycles", "%"),
	.name		= "cycles_percent",
	.cmp		= cycles_percent_cmp,
	.entry		= cycles_percent_entry,
	.width		= 9,
};

static struct c2c_dimension dim_total_stores = {
	.header		= HEADER_BOTH("Store", "count"),
	.name		= "total_stores",
	.cmp		= total_stores_cmp,
	.entry		= total_stores_entry,
	.width		= 7,
};

static struct c2c_dimension dim_cacheline_symbol = {
	.header		= HEADER_LOW("Cacheline"),
	.name		= "cacheline_symbol",
	.cmp		= empty_cmp,
	.entry		= cacheline_symbol_entry,
	.width		= 18,
};

static struct c2c_dimension dim_iaddr_symbol = {
	.header		= HEADER_LOW("Code address"),
	.name		= "iaddr_symbol",
	.cmp		= iaddr_symbol_cmp,
	.entry		= iaddr_symbol_entry,
	.width		= 20,
};

static struct c2c_dimension dim_symbol_view = {
	.header		= HEADER_LOW("Symbol"),
	.name		= "symbol_view",
	.se		= &sort_sym,
	.entry		= symbol_view_entry,
	.width		= SYMBOL_WIDTH,
};

static struct c2c_dimension *function_view_dimensions[] = {
	&dim_iaddr_symbol,
	&dim_cycles_percent,
	&dim_total_stores,
	&dim_cacheline_symbol,
	&dim_symbol_view,
	NULL,
};

static struct c2c_dimension *get_function_dimension(const char *name)
{
	unsigned int i;

	for (i = 0; function_view_dimensions[i]; i++) {
		struct c2c_dimension *dim = function_view_dimensions[i];

		if (!strcmp(dim->name, name))
			return dim;
	}

	return NULL;
}

/* Wrappers so sort_entry-backed dimensions sort/collapse via their se. */
static int64_t c2c_se_cmp(struct perf_hpp_fmt *fmt,
			  struct hist_entry *a, struct hist_entry *b)
{
	struct c2c_fmt *c2c_fmt = container_of(fmt, struct c2c_fmt, fmt);
	struct c2c_dimension *dim = c2c_fmt->dim;

	return dim->se->se_cmp(a, b);
}

static int64_t c2c_se_collapse(struct perf_hpp_fmt *fmt,
			       struct hist_entry *a, struct hist_entry *b)
{
	struct c2c_fmt *c2c_fmt = container_of(fmt, struct c2c_fmt, fmt);
	struct c2c_dimension *dim = c2c_fmt->dim;
	int64_t (*collapse_fn)(struct hist_entry *a, struct hist_entry *b);

	collapse_fn = dim->se->se_collapse ?: dim->se->se_cmp;
	return collapse_fn(a, b);
}

/*
 * Build the c2c_fmt for @name. Returns:
 *   0        and *fmtp set     on success;
 *   -ENOENT  and *fmtp = NULL   if @name is not a function-view dimension
 *                               (caller should fall back to the generic field);
 *   -ENOMEM                     if allocation failed (distinct from -ENOENT so
 *                               the caller does not misreport it as an
 *                               "invalid field").
 */
static int get_function_format(const char *name, struct c2c_fmt **fmtp)
{
	struct c2c_dimension *dim = get_function_dimension(name);
	struct c2c_fmt *c2c_fmt;
	struct perf_hpp_fmt *fmt;

	*fmtp = NULL;

	if (!dim)
		return -ENOENT;

	c2c_fmt = zalloc(sizeof(*c2c_fmt));
	if (!c2c_fmt)
		return -ENOMEM;

	fmt = &c2c_fmt->fmt;

	c2c_fmt->dim = dim;
	INIT_LIST_HEAD(&fmt->list);
	INIT_LIST_HEAD(&fmt->sort_list);

	fmt->cmp	= dim->se ? c2c_se_cmp : dim->cmp;
	fmt->sort	= dim->se ? c2c_se_cmp : dim->cmp;
	fmt->color	= dim->color;
	fmt->entry	= dim->entry;
	fmt->header	= c2c_header;
	fmt->width	= c2c_width;
	fmt->collapse	= dim->se ? c2c_se_collapse : dim->cmp;
	fmt->equal	= fmt_equal;
	fmt->free	= fmt_free;

	*fmtp = c2c_fmt;
	return 0;
}

static int
c2c_function_hists__init_output(struct perf_hpp_list *hpp_list, char *name,
				struct perf_env *env __maybe_unused)
{
	struct c2c_fmt *c2c_fmt;
	int level = 0;
	int ret;

	ret = get_function_format(name, &c2c_fmt);
	if (ret == -ENOMEM)
		return ret;
	if (ret == -ENOENT) {
		reset_dimensions();
		return output_field_add(hpp_list, name, &level);
	}

	/*
	 * Mark symbol-backed columns so hists__has(hists, sym) is correct.
	 * Only dim_symbol_view carries a sort_entry (.se); the function
	 * view's field strings are fixed and always include symbol_view, so
	 * this single check is sufficient (unlike the user-configurable
	 * cacheline view, which must also test dim_iaddr).
	 */
	if (c2c_fmt->dim->se == &sort_sym)
		hpp_list->sym = 1;

	perf_hpp_list__column_register(hpp_list, &c2c_fmt->fmt);
	return 0;
}

static int
c2c_function_hists__init_sort(struct perf_hpp_list *hpp_list, char *name,
			      struct perf_env *env)
{
	struct c2c_fmt *c2c_fmt;
	int ret;

	ret = get_function_format(name, &c2c_fmt);
	if (ret == -ENOMEM)
		return ret;
	if (ret == -ENOENT) {
		reset_dimensions();
		return sort_dimension__add(hpp_list, name, /*evlist=*/NULL, env, /*level=*/0);
	}

	/* Mark symbol-backed sort keys so hists__has(hists, sym) is correct. */
	if (c2c_fmt->dim->se == &sort_sym)
		hpp_list->sym = 1;

	perf_hpp_list__register_sort_field(hpp_list, &c2c_fmt->fmt);
	return 0;
}

typedef int (*hpp_list_add_fn)(struct perf_hpp_list *hpp_list, char *name,
			       struct perf_env *env);

static int function_hpp_list__add_tokens(struct perf_hpp_list *hpp_list, char *list,
					 struct perf_env *env, hpp_list_add_fn add)
{
	char *tok, *tmp;
	int ret;

	if (!list)
		return 0;

	for (tok = strtok_r(list, ", ", &tmp); tok; tok = strtok_r(NULL, ", ", &tmp)) {
		ret = add(hpp_list, tok, env);
		if (ret) {
			if (ret == -EINVAL || ret == -ESRCH)
				pr_err("Invalid c2c function-view field: %s", tok);
			return ret;
		}
	}
	return 0;
}

static int
function_hpp_list__parse(struct perf_hpp_list *hpp_list,
			 const char *output_str,
			 const char *sort_str,
			 struct perf_env *env)
{
	char *output = output_str ? strdup(output_str) : NULL;
	char *sort   = sort_str   ? strdup(sort_str)   : NULL;
	int ret = 0;

	if ((output_str && !output) || (sort_str && !sort)) {
		ret = -ENOMEM;
		goto out;
	}

	ret = function_hpp_list__add_tokens(hpp_list, output, env,
					    c2c_function_hists__init_output);
	if (ret)
		goto out;

	ret = function_hpp_list__add_tokens(hpp_list, sort, env,
					    c2c_function_hists__init_sort);
	if (ret)
		goto out;

	perf_hpp__setup_output_field(hpp_list);
out:
	free(output);
	free(sort);
	return ret;
}

static int
c2c_function_hists__init(struct c2c_hists *hists,
			 const char *sort,
			 int nr_header_lines,
			 struct perf_env *env)
{
	__hists__init(&hists->hists, &hists->list);

	perf_hpp_list__init(&hists->list);

	hists->list.nr_header_lines = nr_header_lines;

	return function_hpp_list__parse(&hists->list, /*output=*/NULL, sort, env);
}

static int
c2c_function_hists__reinit(struct c2c_hists *c2c_hists,
			   const char *output,
			   const char *sort,
			   struct perf_env *env)
{
	int nr_header_lines = c2c_hists->list.nr_header_lines;

	perf_hpp__reset_output_field(&c2c_hists->list);
	INIT_LIST_HEAD(&c2c_hists->list.sorts);

	/* Clear stale state flags so a different output/sort set starts fresh. */
	c2c_hists->list.need_collapse = 0;
	c2c_hists->list.parent = 0;
	c2c_hists->list.sym = 0;
	c2c_hists->list.dso = 0;
	c2c_hists->list.socket = 0;
	c2c_hists->list.thread = 0;
	c2c_hists->list.comm = 0;
	c2c_hists->list.comm_nodigit = 0;
	c2c_hists->list.nr_header_lines = nr_header_lines;

	return function_hpp_list__parse(&c2c_hists->list, output, sort, env);
}

/* Welford online merge of two "stats" (from util/stat.h) accumulators. */
static void c2c_stats_merge(struct stats *dest, const struct stats *src)
{
	double delta;

	if (src->n == 0)
		return;

	if (dest->n == 0) {
		*dest = *src;
		return;
	}

	delta = src->mean - dest->mean;
	dest->M2 += src->M2 + delta * delta * dest->n * src->n / (dest->n + src->n);
	dest->mean = (dest->mean * dest->n + src->mean * src->n) / (dest->n + src->n);
	dest->n += src->n;

	/* Update min/max */
	if (src->max > dest->max)
		dest->max = src->max;
	if (src->min < dest->min)
		dest->min = src->min;
}

/* Merge compute_stats during function aggregation. */
static void c2c_add_cstats(struct compute_stats *dest,
			   const struct compute_stats *src)
{
	c2c_stats_merge(&dest->rmt_hitm, &src->rmt_hitm);
	c2c_stats_merge(&dest->lcl_hitm, &src->lcl_hitm);
	c2c_stats_merge(&dest->rmt_peer, &src->rmt_peer);
	c2c_stats_merge(&dest->lcl_peer, &src->lcl_peer);
	c2c_stats_merge(&dest->load, &src->load);
}

static bool hist_entry__add_c2c_stats(struct hist_entry *he,
				      const struct c2c_stats *stats)
{
	u64 nr_events = c2c_hitm_count(stats) + stats->rmt_peer + stats->lcl_peer;
	u64 weight1 = c2c_hitm_count(stats);

	he->stat.nr_events += nr_events;
	he->stat.period += nr_events;
	he->stat.weight1 += weight1;

	if (!symbol_conf.cumulate_callchain)
		return true;

	if (!he->stat_acc) {
		he->stat_acc = calloc(1, sizeof(struct he_stat));
		if (!he->stat_acc)
			return false;
	}

	he->stat_acc->nr_events += nr_events;
	he->stat_acc->period += nr_events;
	he->stat_acc->weight1 += weight1;

	return true;
}

static void c2c_he__free_hierarchy(struct hist_entry *he);

/*
 * Free a function-view histogram entry (hist_entry_ops::free).
 */
static void c2c_function_he_free(void *ptr)
{
	struct hist_entry *he = ptr;
	struct c2c_hist_entry *c2c_he;

	c2c_he = container_of(he, struct c2c_hist_entry, he);

	if (c2c_he->hists) {
		perf_hpp__reset_output_field(&c2c_he->hists->list);
		hists__delete_all_entries(&c2c_he->hists->hists);
		zfree(&c2c_he->hists);
	}

	c2c_he__free_hierarchy(he);

	zfree(&c2c_he->nodeset);
	zfree(&c2c_he->cpuset);
	zfree(&c2c_he->nodestr);
	zfree(&c2c_he->node_stats);

	free(c2c_he);
}

/*
 * Free all child entries under @he, recursively (hroot_out sub-tree).
 *
 * Children are built by c2c_child_entry__alloc(), which BORROWS thread and
 * ms (plain copy, no thread__get()/map__get()) and OWNS only mem_info (a
 * clone), stat_acc and the c2c-specific fields (hists, cpuset, nodeset,
 * nodestr, node_stats). We therefore must NOT call hist_entry__delete()
 * here: it would thread__zput()/map_symbol__exit() the borrowed refs and
 * underflow their refcounts. Free exactly the owned resources instead.
 */
static void c2c_he__free_hierarchy(struct hist_entry *he)
{
	struct rb_node *nd;
	struct hist_entry *child_he;
	struct c2c_hist_entry *child_c2c;

	/*
	 * Leaf entries alias hroot_out with sorted_chain (callchains) in a
	 * union, so they have no child hierarchy to free here.
	 */
	if (he->leaf)
		return;

	if (RB_EMPTY_ROOT(&he->hroot_out.rb_root))
		return;

	nd = rb_first_cached(&he->hroot_out);
	while (nd) {
		struct rb_node *next = rb_next(nd);

		child_he = rb_entry(nd, struct hist_entry, rb_node);
		child_c2c = container_of(child_he, struct c2c_hist_entry, he);

		if (child_he->stat_acc)
			zfree(&child_he->stat_acc);

		if (child_he->mem_info)
			mem_info__put(child_he->mem_info);

		if (child_c2c->hists) {
			perf_hpp__reset_output_field(&child_c2c->hists->list);
			hists__delete_all_entries(&child_c2c->hists->hists);
			zfree(&child_c2c->hists);
		}

		zfree(&child_c2c->cpuset);
		zfree(&child_c2c->nodeset);
		zfree(&child_c2c->nodestr);
		zfree(&child_c2c->node_stats);

		c2c_he__free_hierarchy(child_he);

		rb_erase_cached(&child_he->rb_node, &he->hroot_out);
		free(child_c2c);

		nd = next;
	}

	/* All children erased; clear the tree (and its cached leftmost). */
	he->hroot_out = RB_ROOT_CACHED;
}

/* Entry operations for function view */
static struct hist_entry_ops c2c_function_entry_ops = {
	.new	= c2c_he_zalloc,
	.free	= c2c_function_he_free,
};

static struct c2c_hist_entry *
c2c_child_entry__alloc(struct hist_entry *parent_he, struct hist_entry *src_he,
		       int depth, u64 ip)
{
	struct c2c_hist_entry *child_c2c;
	struct hist_entry *child_he;
	size_t callchain_size;

	callchain_size = symbol_conf.use_callchain ? sizeof(struct callchain_root) : 0;
	child_he = c2c_he_zalloc(callchain_size);
	if (!child_he)
		return NULL;

	child_c2c = container_of(child_he, struct c2c_hist_entry, he);
	child_he->callchain_size = callchain_size;
	if (callchain_size)
		callchain_init(child_he->callchain);

	memcpy(&child_he->ms, &src_he->ms, sizeof(struct map_symbol));

	if (src_he->mem_info) {
		child_he->mem_info = mem_info__clone(src_he->mem_info);
		if (!child_he->mem_info)
			goto out_free;
	}

	child_he->thread = src_he->thread;
	child_he->cpumode = src_he->cpumode;
	child_he->cpu = src_he->cpu;
	child_he->socket = src_he->socket;
	child_he->level = src_he->level;
	child_he->ip = ip;

	child_he->parent_he = parent_he;
	child_he->depth = depth;
	child_he->leaf = (depth >= 2);
	child_he->hists = &c2c_ext.function_hists.hists;
	child_he->filtered = false;
	child_he->unfolded = false;
	child_he->has_children = false;
	child_he->has_no_entry = false;
	child_he->nr_rows = 0;
	child_he->row_offset = 0;

	memset(&child_he->stat, 0, sizeof(child_he->stat));
	child_he->hroot_in = RB_ROOT_CACHED;
	child_he->hroot_out = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&child_he->pairs.node);
	child_he->hpp_list = &c2c_ext.function_hists.list;
	if (symbol_conf.cumulate_callchain) {
		child_he->stat_acc = calloc(1, sizeof(struct he_stat));
		if (!child_he->stat_acc)
			goto out_free;
	}

	return child_c2c;

out_free:
	if (child_he->mem_info)
		mem_info__put(child_he->mem_info);
	zfree(&child_c2c->cpuset);
	zfree(&child_c2c->nodeset);
	zfree(&child_c2c->node_stats);
	free(child_c2c);
	return NULL;
}

static void
c2c_child_entry__insert(struct hist_entry *parent_he, struct hist_entry *child_he,
			struct rb_node **p, struct rb_node *rb_parent, bool leftmost)
{
	rb_link_node(&child_he->rb_node, rb_parent, p);
	rb_insert_color_cached(&child_he->rb_node, &parent_he->hroot_out, leftmost);

	parent_he->has_children = true;
	parent_he->leaf = false;
}

static struct hist_entry *
c2c_function_hists__level1_entry(struct symbol *sym, u64 iaddr,
				 struct hist_entry *detail_he,
				 struct thread *synthetic_thread)
{
	struct addr_location al;
	struct perf_sample sample = {};
	struct mem_info *mi;
	struct hist_entry *he;

	mi = mem_info__new();
	if (mi) {
		mem_info__iaddr(mi)->addr = iaddr;
		/* mem_info__put() will map_symbol__exit() these, so take refs. */
		mem_info__iaddr(mi)->ms.thread = thread__get(detail_he->ms.thread);
		mem_info__iaddr(mi)->ms.map = map__get(detail_he->ms.map);
		mem_info__iaddr(mi)->ms.sym = sym;
		mem_info__daddr(mi)->addr = 0;
	}

	addr_location__init(&al);
	al.thread = thread__get(synthetic_thread);
	al.map = map__get(detail_he->ms.map);
	al.sym = sym;
	al.addr = iaddr;
	al.level = detail_he->level;
	al.cpumode = detail_he->cpumode;
	al.cpu = 0;
	al.socket = 0;
	al.filtered = 0;
	al.latency = 0;

	/*
	 * Synthetic sample: period/weight are placeholders only. The real
	 * c2c counters live in c2c_hist_entry::stats and are added via
	 * hist_entry__add_c2c_stats(); no function-view column or sort key
	 * reads he->stat.period/nr_events, so the +1 that __hists__add_entry()
	 * accrues on each dedup hit has no effect on what is displayed.
	 */
	sample.period = 1;
	sample.weight = 1;
	sample.ip = iaddr;
	sample.pid = thread__pid(synthetic_thread);
	sample.tid = thread__tid(synthetic_thread);
	sample.cpu = 0;

	/* Add entry - histogram handles dedup */
	he = hists__add_entry_ops(&c2c_ext.function_hists.hists,
				  &c2c_function_entry_ops,
				  &al, NULL, NULL, mi,
				  NULL, &sample, true);

	addr_location__exit(&al);
	if (mi)
		mem_info__put(mi);

	if (he)
		he->hpp_list = &c2c_ext.function_hists.list;

	return he;
}

static struct c2c_hist_entry *
c2c_function_hists__level2_entry(struct c2c_hist_entry *level1_c2c,
				 struct symbol *sym, u64 iaddr,
				 struct hist_entry *detail_he)
{
	struct hist_entry *level1_he = &level1_c2c->he;
	struct rb_node **p = &level1_he->hroot_out.rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct c2c_hist_entry *level2_c2c;
	bool leftmost = true;

	/*
	 * Order by (iaddr, symbol name). Symbols are looked up by name to
	 * coalesce identically-named symbols from different DSO/JIT copies,
	 * which matches the dedup policy used when building the hierarchy.
	 */
	while (*p) {
		struct hist_entry *iter = rb_entry(*p, struct hist_entry, rb_node);
		u64 iter_iaddr = hist_entry__iaddr(iter);
		int cmp;

		parent = *p;
		if (iaddr < iter_iaddr) {
			p = &parent->rb_left;
			continue;
		}
		if (iaddr > iter_iaddr) {
			p = &parent->rb_right;
			leftmost = false;
			continue;
		}

		if (!sym || !iter->ms.sym)
			/* Order NULL-symbol entries deterministically (NULL last). */
			cmp = (iter->ms.sym ? 1 : 0) - (sym ? 1 : 0);
		else
			cmp = arch__compare_symbol_names(sym->name, iter->ms.sym->name);

		if (cmp < 0) {
			p = &parent->rb_left;
		} else if (cmp > 0) {
			p = &parent->rb_right;
			leftmost = false;
		} else {
			return container_of(iter, struct c2c_hist_entry, he);
		}
	}

	level2_c2c = c2c_child_entry__alloc(level1_he, detail_he, 1, iaddr);
	if (!level2_c2c)
		return NULL;

	/* Key this level by the looked-up symbol, not detail_he's. */
	level2_c2c->he.ms.sym = sym;

	/* Override iaddr (and symbol) in cloned mem_info for level 2 */
	if (level2_c2c->he.mem_info) {
		mem_info__iaddr(level2_c2c->he.mem_info)->addr = iaddr;
		mem_info__iaddr(level2_c2c->he.mem_info)->ms.sym = sym;
	}

	c2c_child_entry__insert(level1_he, &level2_c2c->he, p, parent, leftmost);

	return level2_c2c;
}

static struct c2c_hist_entry *
c2c_function_hists__level3_entry(struct c2c_hist_entry *level2_c2c, u64 cl_addr,
				 struct c2c_hist_entry *cacheline_src_he)
{
	struct hist_entry *level2_he = &level2_c2c->he;
	struct rb_node **p = &level2_he->hroot_out.rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct c2c_hist_entry *level3_c2c;
	bool leftmost = true;

	while (*p) {
		struct hist_entry *iter = rb_entry(*p, struct hist_entry, rb_node);
		u64 iter_addr = 0;

		if (iter->mem_info) {
			u64 daddr = mem_info__daddr(iter->mem_info)->addr;

			iter_addr = cl_address(daddr, chk_double_cl);
		}

		parent = *p;
		if (cl_addr < iter_addr) {
			p = &parent->rb_left;
		} else if (cl_addr > iter_addr) {
			p = &parent->rb_right;
			leftmost = false;
		} else {
			return container_of(iter, struct c2c_hist_entry, he);
		}
	}

	level3_c2c = c2c_child_entry__alloc(level2_he, &cacheline_src_he->he, 2,
					    hist_entry__iaddr(&cacheline_src_he->he));
	if (!level3_c2c)
		return NULL;

	c2c_child_entry__insert(level2_he, &level3_c2c->he, p, parent, leftmost);

	return level3_c2c;
}

/*
 * Re-sort child entries of @parent_he by total store count, descending.
 */
static void c2c_he__resort_by_stores(struct hist_entry *parent_he)
{
	struct rb_root_cached new_root = RB_ROOT_CACHED;
	struct rb_node *nd;

	if (!parent_he->has_children)
		return;

	/* Extract all nodes and re-insert sorted by total_stores */
	while ((nd = rb_first_cached(&parent_he->hroot_out))) {
		struct hist_entry *he = rb_entry(nd, struct hist_entry, rb_node);
		struct c2c_hist_entry *c2c_he = container_of(he, struct c2c_hist_entry, he);
		struct rb_node **p = &new_root.rb_root.rb_node;
		struct rb_node *parent = NULL;
		bool leftmost = true;
		int cmp;

		/* Remove from current tree */
		rb_erase_cached(&he->rb_node, &parent_he->hroot_out);

		/* Insert sorted by store count, descending. */
		while (*p) {
			struct hist_entry *iter = rb_entry(*p, struct hist_entry, rb_node);
			struct c2c_hist_entry *c2c_iter = container_of(iter,
								       struct c2c_hist_entry,
								       he);

			parent = *p;
			if (c2c_he->stats.store != c2c_iter->stats.store) {
				cmp = c2c_he->stats.store > c2c_iter->stats.store ? -1 : 1;
			} else {
				/* Stable tie-break: instruction address, then name. */
				u64 a = hist_entry__iaddr(he), b = hist_entry__iaddr(iter);

				if (a != b)
					cmp = a < b ? -1 : 1;
				else if (he->ms.sym && iter->ms.sym)
					cmp = arch__compare_symbol_names(he->ms.sym->name,
									 iter->ms.sym->name);
				else
					cmp = (iter->ms.sym ? 1 : 0) - (he->ms.sym ? 1 : 0);
			}

			if (cmp < 0) {
				p = &parent->rb_left;
			} else {
				p = &parent->rb_right;
				leftmost = false;
			}
		}

		rb_link_node(&he->rb_node, parent, p);
		rb_insert_color_cached(&he->rb_node, &new_root, leftmost);
	}

	parent_he->hroot_out = new_root;
}

/* Initial per-cacheline capacity for the seen[] set; grown on demand. */
#define DEFAULT_SYMBOLS_PER_CL 64

struct function_seen {
	struct symbol	*sym;
	u64		 iaddr;
};

static bool function_seen__find(const struct function_seen *seen, int nr,
				struct symbol *sym, u64 iaddr)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (seen[i].iaddr == iaddr &&
		    symbol_name_equal(seen[i].sym, sym))
			return true;
	}
	return false;
}

/* Aggregate stats from the cacheline-side entry @c2c_b into level 2/3 @dst. */
static bool c2c_he__add_sharing(struct c2c_hist_entry *dst, struct c2c_hist_entry *src)
{
	/* Do the fallible update first so a failure leaves dst unmodified. */
	if (!hist_entry__add_c2c_stats(&dst->he, &src->stats))
		return false;

	c2c_add_stats(&dst->stats, &src->stats);
	c2c_add_cstats(&dst->cstats, &src->cstats);
	return true;
}

/*
 * Process one cacheline and create/update the level-1/2/3 hierarchy entries
 * for every pair of functions sharing it.
 */
static int c2c_function__process_cl(struct c2c_hist_entry *cacheline_he, u64 cl_addr,
				    struct thread *synthetic_thread)
{
	struct rb_node *nd_a, *nd_b;
	struct function_seen *seen = NULL;
	int nr_seen = 0, nr_alloc = 0;
	int ret = 0;

	for (nd_a = rb_first_cached(&cacheline_he->hists->hists.entries); nd_a;
	     nd_a = rb_next(nd_a)) {
		struct hist_entry *he_a = rb_entry(nd_a, struct hist_entry, rb_node);
		struct c2c_hist_entry *c2c_a;
		struct hist_entry *level1_he;
		struct c2c_hist_entry *level1_c2c;
		u64 iaddr_a;

		if (!he_a->ms.sym || he_a->filtered)
			continue;

		c2c_a = container_of(he_a, struct c2c_hist_entry, he);
		iaddr_a = hist_entry__iaddr(he_a);

		level1_he = c2c_function_hists__level1_entry(he_a->ms.sym, iaddr_a,
							     he_a, synthetic_thread);
		if (!level1_he) {
			ret = -ENOMEM;
			goto out;
		}

		level1_c2c = container_of(level1_he, struct c2c_hist_entry, he);

		/*
		 * Aggregate every source entry into its level-1 (sym, iaddr)
		 * parent. level1_he is keyed by (sym, iaddr), so all siblings
		 * collapse into the same parent. When the cacheline view splits
		 * one (sym, iaddr) into siblings (only under -d pid/tid/dso),
		 * each sibling holds a DISJOINT slice of the traffic, so summing
		 * them here is correct accumulation, not double counting. The
		 * seen[] set below therefore guards only the inner B-loop (to
		 * avoid building a level-2 subtree twice), never this L1 update.
		 * Update he->stat first; on failure leave the aggregates untouched.
		 */
		if (!hist_entry__add_c2c_stats(level1_he, &c2c_a->stats)) {
			ret = -ENOMEM;
			goto out;
		}
		c2c_add_stats(&level1_c2c->stats, &c2c_a->stats);
		c2c_add_cstats(&level1_c2c->cstats, &c2c_a->cstats);
		c2c_add_stats(&c2c_ext.function_hists.stats, &c2c_a->stats);

		/* Skip the inner loop when this (symbol, iaddr) is already a parent. */
		if (function_seen__find(seen, nr_seen, he_a->ms.sym, iaddr_a))
			continue;

		if (nr_seen == nr_alloc) {
			struct function_seen *tmp;
			int new_alloc = nr_alloc ? nr_alloc * 2 : DEFAULT_SYMBOLS_PER_CL;

			tmp = reallocarray(seen, new_alloc, sizeof(*seen));
			if (!tmp) {
				ret = -ENOMEM;
				goto out;
			}
			seen = tmp;
			nr_alloc = new_alloc;
		}
		seen[nr_seen].sym = he_a->ms.sym;
		seen[nr_seen].iaddr = iaddr_a;
		nr_seen++;

		for (nd_b = rb_first_cached(&cacheline_he->hists->hists.entries); nd_b;
		     nd_b = rb_next(nd_b)) {
			struct hist_entry *he_b = rb_entry(nd_b, struct hist_entry, rb_node);
			struct c2c_hist_entry *c2c_b, *level2_c2c, *level3_c2c;
			u64 iaddr_b;

			if (!he_b->ms.sym || he_b->filtered)
				continue;

			c2c_b = container_of(he_b, struct c2c_hist_entry, he);
			iaddr_b = hist_entry__iaddr(he_b);

			/* Skip self. */
			if (iaddr_a == iaddr_b &&
			    symbol_name_equal(he_a->ms.sym, he_b->ms.sym))
				continue;

			level2_c2c = c2c_function_hists__level2_entry(level1_c2c, he_b->ms.sym,
								      iaddr_b, he_b);
			if (!level2_c2c || !c2c_he__add_sharing(level2_c2c, c2c_b)) {
				ret = -ENOMEM;
				goto out;
			}

			/*
			 * Cache the running sum of level-2 store counts on the
			 * level-1 parent, mirroring c2c_he__add_sharing() above,
			 * so hist_entry__child_stores() stays O(1).
			 */
			level1_c2c->child_stores += c2c_b->stats.store;

			level3_c2c = c2c_function_hists__level3_entry(level2_c2c, cl_addr,
								      cacheline_he);
			if (!level3_c2c) {
				ret = -ENOMEM;
				goto out;
			}

			if (!c2c_he__add_sharing(level3_c2c, c2c_b)) {
				ret = -ENOMEM;
				goto out;
			}
		}
	}

out:
	free(seen);
	return ret;
}

/* Sort level-2/3 children by store count, then compute the global total. */
static void c2c_function__finalize(void)
{
	struct rb_node *nd_l1;

	for (nd_l1 = rb_first_cached(&c2c_ext.function_hists.hists.entries); nd_l1;
	     nd_l1 = rb_next(nd_l1)) {
		struct hist_entry *he_l1 = rb_entry(nd_l1, struct hist_entry, rb_node);
		struct rb_node *nd_l2;

		if (!he_l1->has_children)
			continue;

		c2c_he__resort_by_stores(he_l1);

		for (nd_l2 = rb_first_cached(&he_l1->hroot_out); nd_l2;
		     nd_l2 = rb_next(nd_l2)) {
			struct hist_entry *he_l2 = rb_entry(nd_l2, struct hist_entry, rb_node);

			if (he_l2->has_children)
				c2c_he__resort_by_stores(he_l2);
		}
	}

	c2c_ext.total_cycles = c2c_ext__total_cycles();
}

/*
 * Build the three-level function view in a single pass over the cacheline
 * entries:
 *   L1: aggregate stats per primary function
 *   L2: sharing functions referenced from each L1 function
 *   L3: cachelines that pair L1 with L2
 */
static int build_function_view_hierarchy(void)
{
	static const char output_fields[] =
		"cycles_percent,total_stores,iaddr_symbol,symbol_view,cacheline_symbol";
	struct rb_node *nd_cl;
	int ret;

	c2c_ext.total_cycles = 0;
	memset(&c2c_ext.function_hists.stats, 0,
	       sizeof(c2c_ext.function_hists.stats));

	hists__delete_entries(&c2c_ext.function_hists.hists);
	if (c2c_ext.function_hists.list.fields.next)
		perf_hpp__reset_output_field(&c2c_ext.function_hists.list);

	ret = c2c_function_hists__init(&c2c_ext.function_hists,
				       "iaddr_symbol,symbol_view", 2, NULL);
	if (ret)
		return ret;

	nd_cl = rb_first_cached(&c2c.hists.hists.entries);

	/* An empty C2C report yields an empty (but valid) function view. */
	for (; nd_cl; nd_cl = rb_next(nd_cl)) {
		struct hist_entry *he_cl = rb_entry(nd_cl, struct hist_entry, rb_node);
		struct c2c_hist_entry *cacheline_he = container_of(he_cl,
								   struct c2c_hist_entry, he);
		struct thread *synthetic_thread = he_cl->thread;
		u64 cl_addr;

		/*
		 * Include any cacheline with sharing activity (HITM, peer,
		 * stores or loads), not just HITM, so totals/sorting reflect
		 * all aggregated traffic surfaced by the function view.
		 */
		if ((c2c_hitm_count(&cacheline_he->stats) == 0 &&
		     cacheline_he->stats.tot_peer == 0 &&
		     cacheline_he->stats.store == 0 &&
		     cacheline_he->stats.load == 0) ||
		    !cacheline_he->hists ||
		    RB_EMPTY_ROOT(&cacheline_he->hists->hists.entries.rb_root) ||
		    !he_cl->mem_info || !synthetic_thread)
			continue;

		cl_addr = cl_address(mem_info__daddr(he_cl->mem_info)->addr, chk_double_cl);
		ret = c2c_function__process_cl(cacheline_he, cl_addr, synthetic_thread);
		if (ret)
			goto out_err;
	}

	ret = c2c_function_hists__reinit(&c2c_ext.function_hists, output_fields,
					 "cycles_percent", NULL);
	if (ret)
		goto out_err;

	hists__collapse_resort(&c2c_ext.function_hists.hists, NULL);
	hists__output_resort(&c2c_ext.function_hists.hists, NULL);

	c2c_function__finalize();

	return 0;

out_err:
	/*
	 * On error, migrate any entries still in entries_in to entries and
	 * delete them, so a later rebuild does not strand them (the top-level
	 * __hists__init() memset would otherwise lose the pointers).
	 */
	hists__collapse_resort(&c2c_ext.function_hists.hists, NULL);
	hists__output_resort(&c2c_ext.function_hists.hists, NULL);
	hists__delete_entries(&c2c_ext.function_hists.hists);
	return ret;
}

static int c2c_function_browser__title(struct hist_browser *browser,
				       char *bf, size_t size)
{
	scnprintf(bf, size,
		  "Shared Data Functions Table     (%" PRIu64 " entries, sorted on Cycles %%)",
		  browser->nr_non_filtered_entries);
	return 0;
}

static struct c2c_function_browser *c2c_function_browser__new(struct hists *hists)
{
	struct c2c_function_browser *browser;

	if (!hists)
		return NULL;

	browser = zalloc(sizeof(*browser));
	if (!browser)
		return NULL;

	hist_browser__init(&browser->hb, hists);

	browser->hb.title = c2c_function_browser__title;
	browser->hb.c2c_filter = true;
	browser->hb.show_headers = true;
	/* Keep title line count consistent with forcing headers on. */
	browser->hb.b.extra_title_lines = hists->hpp_list->nr_header_lines;
	browser->hb.min_pcnt = 0.0;

	/*
	 * Note: symbol_conf.report_hierarchy is deliberately left unset.
	 * The generic browser still descends into hroot_out children via
	 * rb_hierarchy_next()/can_goto_child(), which key off he->unfolded,
	 * so 'e'/'+' expands L1 -> L2 -> L3 correctly. Setting the flag would
	 * additionally make hist_entry__delete() recurse hroot_out and free
	 * each child, but our children borrow thread/ms (see
	 * c2c_child_entry__alloc()), so that would underflow their refcounts.
	 * Teardown is handled by c2c_he__free_hierarchy() instead.
	 */
	return browser;
}

/*
 * c2c_function_browser__delete - Free function browser
 */
static void c2c_function_browser__delete(struct c2c_function_browser *browser)
{
	free(browser);
}

static int c2c_function_browser__browse_cacheline_detail(struct hist_entry *he_selection,
							 struct hists *hists)
{
	struct rb_node *nd;
	u64 cl_addr;

	if (!he_selection || !he_selection->parent_he ||
	    !he_selection->parent_he->parent_he || !he_selection->mem_info)
		return -1;

	cl_addr = cl_address(mem_info__daddr(he_selection->mem_info)->addr, chk_double_cl);

	for (nd = rb_first_cached(&hists->entries); nd; nd = rb_next(nd)) {
		struct hist_entry *he_cl = rb_entry(nd, struct hist_entry, rb_node);
		u64 this_cl;

		if (!he_cl->mem_info)
			continue;

		this_cl = cl_address(mem_info__daddr(he_cl->mem_info)->addr, chk_double_cl);
		if (this_cl == cl_addr)
			return perf_c2c__browse_cacheline(he_cl);
	}

	return -1;
}

/*
 * perf_c2c__browse_function_view - Browse function view with TAB key support
 * @hists: Main cacheline histograms
 *
 * Returns: 0 on success, negative error code on failure
 */
int perf_c2c__browse_function_view(struct hists *hists)
{
	struct c2c_function_browser *sym_browser;
	bool saved_use_callchain = symbol_conf.use_callchain;
	int key, ret;
	static const char help[] =
	" d             Display cacheline details for the selected entry\n"
	" e/+           Expand/collapse the selected entry\n"
	" TAB/ESC/q     Return to the cacheline view\n";

	if (!hists)
		return -EINVAL;

	/* Disable callchain before building so no callchain structs are allocated. */
	symbol_conf.use_callchain = false;

	ret = build_function_view_hierarchy();
	if (ret) {
		ui__error("Failed to build function view hierarchy (ret=%d)\n", ret);
		goto out;
	}

	sym_browser = c2c_function_browser__new(&c2c_ext.function_hists.hists);
	if (!sym_browser) {
		ret = -ENOMEM;
		goto out;
	}

	/* Reset abort key so we can receive Ctrl-C as a key. */
	SLang_reset_tty();
	SLang_init_tty(0, 0, 0);
	SLtty_set_suspend_state(true);

	sym_browser->hb.nr_non_filtered_entries =
		c2c_ext.function_hists.hists.nr_non_filtered_entries;

	while (1) {
		key = hist_browser__run(&sym_browser->hb, "? - help", true, 0);

		switch (key) {
		case 'q':
		case K_TAB:
		case K_ESC:
			goto browser_done;
		case 'd':
			/* Cacheline detail honors the user's callchain setting. */
			symbol_conf.use_callchain = saved_use_callchain;
			c2c_function_browser__browse_cacheline_detail(sym_browser->hb.he_selection,
								      hists);
			/* Preserve any toggle made in the detail view, then
			 * re-disable callchain for the function view.
			 */
			saved_use_callchain = symbol_conf.use_callchain;
			symbol_conf.use_callchain = false;
			break;
		case '?':
			ui_browser__help_window(&sym_browser->hb.b, help);
			break;
		default:
			break;
		}
	}

browser_done:
	c2c_function_browser__delete(sym_browser);
out:
	symbol_conf.use_callchain = saved_use_callchain;
	return ret;
}
