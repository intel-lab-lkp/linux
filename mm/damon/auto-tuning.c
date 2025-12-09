// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON-based auto tuning module.
 *
 * Author: Enze Li <lienze@kylinos.cn>
 * Copyright (C) 2025 KylinSoft Corporation
 */

#define pr_fmt(fmt) "damon-auto-tuning: " fmt

#include <linux/damon.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/list_sort.h>

#include "auto-tuning.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_auto_tuning."

static bool init_auto_tuning;
static int auto_targets;
static int max_nr_targets;
static unsigned long init_metric;
struct damon_ctx *auto_ctx;

static struct damos *damon_auto_tuning_new_scheme(void)
{
	struct damos_access_pattern auto_pattern = {
		.min_sz_region = PAGE_SIZE,
		.max_sz_region = ULONG_MAX,
		.min_nr_accesses = 0,
		.max_nr_accesses = 0,
		.min_age_region = 12000000 / 100000,
		.max_age_region = UINT_MAX,
	};

	struct damos_quota auto_quota = {
		.ms = 10,
		.sz = 128 * 1024 * 1024,
		.reset_interval = 1000,
		.weight_sz = 0,
		.weight_nr_accesses = 0,
		.weight_age = 1
	};

	struct damos_watermarks auto_wmarks = {
		.metric = DAMOS_WMARK_FREE_MEM_RATE,
		.interval = 5000000,
		.high = 500,
		.mid = 400,
		.low = 200,
	};

	return damon_new_scheme(&auto_pattern, DAMOS_PAGEOUT, 0, &auto_quota,
				&auto_wmarks, NUMA_NO_NODE);
}

static bool damon_target_filter_match(struct damon_ctx *c,
				      struct damos_filter *filter,
				      struct damon_target *t)
{
	struct damon_target *ti;
	int target_idx = 0;

	damon_for_each_target(ti, c) {
		if (ti == t)
			break;
		target_idx++;
	}
	return target_idx == filter->target_idx;
}

static int damon_nr_targets(struct damon_ctx *c)
{
	struct damon_target *t;
	int nr_targets = 0;

	damon_for_each_target(t, c) {
		if (c->ops.target_valid && c->ops.target_valid(t))
			nr_targets++;
	}
	return nr_targets;
}

static bool damon_targets_print_info(struct damon_ctx *c)
{
	int i = 0;
	struct damos *s, *next;

	damon_for_each_scheme_safe(s, next, c) {
		pr_info("scheme%d:\n", ++i);
		pr_info("\t wmarks {%d %ld %ld %ld %ld %d}\n",
			s->wmarks.metric, s->wmarks.interval,
			s->wmarks.high, s->wmarks.mid, s->wmarks.low,
			s->wmarks.activated);
	}
	return true;
}

static int damon_targets_priority_max(struct damon_ctx *c)
{
	struct damon_target *t;
	int priority_max = 0;

	damon_for_each_target(t, c) {
		if (c->ops.target_valid && c->ops.target_valid(t))
			if (t->priority > priority_max)
				priority_max = t->priority;
	}
	pr_debug("%s max=%d\n", __func__, priority_max);
	return priority_max;
}

static int damon_targets_priority_min(struct damon_ctx *c)
{
	struct damon_target *t;
	int priority_min = INT_MAX;

	damon_for_each_target(t, c) {
		if (c->ops.target_valid && c->ops.target_valid(t))
			if (t->priority < priority_min)
				priority_min = t->priority;
	}
	pr_debug("%s min=%d\n", __func__, priority_min);
	return priority_min;
}

static bool _damon_auto_tuning_wmarks(struct damon_ctx *c, struct damos *s,
				      struct damon_target *t)
{
	unsigned long adjust_priority, metric;
	int min = damon_targets_priority_min(c);
	int max = damon_targets_priority_max(c);
	int nr_targets = damon_nr_targets(c);
	int decay_factor = max_nr_targets - nr_targets;

	metric = global_zone_page_state(NR_FREE_PAGES) * 1000 /
		 totalram_pages();

	/*
	 * We are now ready to start adjusting the watermarks based on
	 * priority.  The goal is to set each target's watermark above the
	 * system's high watermark (to prevent it from triggering system-wide
	 * actions) and below its initial memory capacity, as determined by
	 * its importance.
	 */
	pr_debug("%s: metric=%ld\n", __func__, metric);
	/* TODO: Here, we need to use the system's high watermark value, and
	 * will temporarily substitute it with 100.
	 */
	if (nr_targets < max_nr_targets)
		adjust_priority = adjust_priority * decay_factor / 10;
	adjust_priority = (t->priority - min) * (metric - 100) / (max - min) + 100;

	s->wmarks.high = adjust_priority;
	s->wmarks.mid = adjust_priority;
	s->wmarks.low = 0;
	return true;
}

static bool damon_auto_tuning_wmarks(struct damon_ctx *c)
{
	struct damon_target *t;
	struct damos *s, *next;
	struct damos_filter *f;

	damon_for_each_target(t, c) {
		if (c->ops.target_valid && c->ops.target_valid(t)) {
			damon_for_each_scheme_safe(s, next, c) {
				damos_for_each_core_filter(f, s) {
					if (damon_target_filter_match(c, f, t)) {
						pr_debug("%s: priority=%d(idx=%d)\n",
							 __func__, t->priority,
							 f->target_idx);
						_damon_auto_tuning_wmarks(c, s, t);
					}
				}
			}
		}
	}
	damon_targets_print_info(c);
	return true;
}

static bool damon_targets_auto_tuning_tick(struct damon_ctx *c,
					   bool init_wmarks)
{
	int nr_targets = 0;
	unsigned long cur_jiffies = jiffies;
	static unsigned long last_jiffies;

	if (last_jiffies != 0) {
		if (time_is_after_jiffies(last_jiffies + 5 * HZ))
			return false;
	}

	pr_debug("%s %ld %ld\n", __func__, cur_jiffies, last_jiffies + 5 * HZ);
	last_jiffies = cur_jiffies;

	/*
	 * Any change in the number of targets necessitates a recalibration of
	 * the per-task watermarks.
	 */
	nr_targets = damon_nr_targets(auto_ctx);
	if (auto_targets != nr_targets) {
		damon_auto_tuning_wmarks(auto_ctx);
		auto_targets = nr_targets;
	}

	if (max_nr_targets < auto_targets)
		max_nr_targets = auto_targets;

	return true;
}

static bool damon_targets_auto_tuning_init(struct damon_ctx *c)
{
	struct damon_target *t;
	struct damos *s, *next, *new_scheme;
	struct damos_filter *new_filter;
	int i = 0;

	init_auto_tuning = true;
	auto_ctx = c;

	damon_for_each_scheme_safe(s, next, c)
		damon_destroy_scheme(s);

	damon_for_each_target(t, c) {
		new_scheme = damon_auto_tuning_new_scheme();
		if (!new_scheme)
			return -ENOMEM;

		new_filter = damos_new_filter(DAMOS_FILTER_TYPE_TARGET, true, true);
		if (!new_filter)
			return -ENOMEM;
		new_filter->target_idx = i++;

		damos_add_filter(new_scheme, new_filter);
		damon_add_scheme(c, new_scheme);

		pr_info("\t add scheme %d\n", auto_targets);
		auto_targets++;
	}

	max_nr_targets = auto_targets;

	damon_targets_print_info(c);
	damon_auto_tuning_wmarks(c);
	damon_targets_print_info(c);

	return true;
}

static bool damon_targets_priority_enabled(struct damon_ctx *c)
{
	struct damon_target *t;

	damon_for_each_target(t, c)
		if (t->priority > 0)
			return true;
	return false;
}

bool kdamond_targets_auto_tuning(struct damon_ctx *c)
{
	if (damon_targets_priority_enabled(c)) {
		if (!init_auto_tuning)
			damon_targets_auto_tuning_init(c);
		else
			damon_targets_auto_tuning_tick(c, false);
	}
	return false;
}

static int __init damon_auto_tuning_init(void)
{
	int err;

	if (!damon_initialized()) {
		err = -ENOMEM;
		goto out;
	}
	init_metric = global_zone_page_state(NR_FREE_PAGES) * 1000 /
		      totalram_pages();
	pr_info("%s init_metric=%ld\n", __func__,  init_metric);
out:
	return err;
}

module_init(damon_auto_tuning_init);
