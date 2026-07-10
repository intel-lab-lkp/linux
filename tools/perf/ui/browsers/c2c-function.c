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

static __maybe_unused int
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

static __maybe_unused int
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
static __maybe_unused void c2c_add_cstats(struct compute_stats *dest,
			   const struct compute_stats *src)
{
	c2c_stats_merge(&dest->rmt_hitm, &src->rmt_hitm);
	c2c_stats_merge(&dest->lcl_hitm, &src->lcl_hitm);
	c2c_stats_merge(&dest->rmt_peer, &src->rmt_peer);
	c2c_stats_merge(&dest->lcl_peer, &src->lcl_peer);
	c2c_stats_merge(&dest->load, &src->load);
}

static __maybe_unused bool hist_entry__add_c2c_stats(struct hist_entry *he,
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
static struct hist_entry_ops c2c_function_entry_ops __maybe_unused = {
	.new	= c2c_he_zalloc,
	.free	= c2c_function_he_free,
};

int perf_c2c__browse_function_view(struct hists *hists __maybe_unused)
{
	ui__warning("C2C function view is not implemented yet.\n");
	return -ENOSYS;
}
