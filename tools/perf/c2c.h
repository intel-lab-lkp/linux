/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PERF_C2C_H_
#define _PERF_C2C_H_ 1

#include <stdbool.h>
#include <stddef.h>
#include <linux/types.h>
#include "util/stat.h"
#include "util/hist.h"
#include "util/mem-events.h"
#include "util/mem2node.h"
#include "util/tool.h"

struct sort_entry;

struct compute_stats {
	struct stats		 lcl_hitm;
	struct stats		 rmt_hitm;
	struct stats		 lcl_peer;
	struct stats		 rmt_peer;
	struct stats		 load;
};

struct c2c_hists {
	struct hists		hists;
	struct perf_hpp_list	list;
	struct c2c_stats	stats;
};

struct c2c_hist_entry {
	struct c2c_hists	*hists;
	struct evsel		*evsel;
	struct c2c_stats	 stats;
	unsigned long		*cpuset;
	unsigned long		*nodeset;
	struct c2c_stats	*node_stats;
	unsigned int		 cacheline_idx;

	struct compute_stats	 cstats;

	unsigned long		 paddr;
	unsigned long		 paddr_cnt;
	bool			 paddr_zero;
	char			*nodestr;

	/*
	 * Function view only: cached sum of the level-2 children's store
	 * counts, accumulated while the hierarchy is built. Lets the
	 * total-stores column and its sort comparator avoid an O(children)
	 * walk on every access. Unused by the cacheline view.
	 */
	u64			 child_stores;

	/*
	 * must be at the end,
	 * because of its callchain dynamic entry
	 */
	struct hist_entry	he;
};

struct perf_c2c {
	struct perf_tool	tool;
	struct c2c_hists	hists;
	struct mem2node		mem2node;

	unsigned long		**nodes;
	int			 nodes_cnt;
	int			 cpus_cnt;
	int			*cpu2node;
	int			 node_info;

	bool			 show_src;
	bool			 show_all;
	bool			 use_stdio;
	bool			 stats_only;
	bool			 symbol_full;
	bool			 stitch_lbr;

	/* Shared cache line stats */
	struct c2c_stats	shared_clines_stats;
	int			shared_clines;

	int			 display;

	const char		*coalesce;
	char			*cl_sort;
	char			*cl_resort;
	char			*cl_output;
};

extern struct perf_c2c c2c;

#define C2C_HEADER_MAX 2
#define SYMBOL_WIDTH 30

#define HEADER_LOW(__h)			\
	{				\
		.line[1] = {		\
			.text = __h,	\
		},			\
	}

#define HEADER_BOTH(__h0, __h1)		\
	{				\
		.line[0] = {		\
			.text = __h0,	\
		},			\
		.line[1] = {		\
			.text = __h1,	\
		},			\
	}

struct c2c_header {
	struct {
		const char *text;
		int	    span;
	} line[C2C_HEADER_MAX];
};

struct c2c_dimension {
	struct c2c_header	 header;
	const char		*name;
	int			 width;
	struct sort_entry	*se;

	int64_t	(*cmp)(struct perf_hpp_fmt *fmt,
		       struct hist_entry *left, struct hist_entry *right);
	int	(*entry)(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
			 struct hist_entry *he);
	int	(*color)(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
			 struct hist_entry *he);
};

struct c2c_fmt {
	struct perf_hpp_fmt	 fmt;
	struct c2c_dimension	*dim;
};

void *c2c_he_zalloc(size_t size);
void fmt_free(struct perf_hpp_fmt *fmt);
bool fmt_equal(struct perf_hpp_fmt *a, struct perf_hpp_fmt *b);

#ifdef HAVE_SLANG_SUPPORT
int perf_c2c__browse_cacheline(struct hist_entry *he);
int perf_c2c__browse_function_view(struct hists *hists);
#endif

#endif /* _PERF_C2C_H_ */
