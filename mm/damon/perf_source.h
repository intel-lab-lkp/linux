/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON perf-event source - public interface
 *
 * Callers that register perf-event probes include this header.
 */

#ifndef _DAMON_PERF_SOURCE_H
#define _DAMON_PERF_SOURCE_H

#ifdef CONFIG_DAMON_PERF_SOURCE

#include <linux/damon.h>
#include <linux/perf_event.h>

struct damon_perf_probe_event;

int damon_perf_probe_setup(struct damon_ctx *ctx,
			   struct damon_probe *probe,
			   struct damon_perf_probe_event *event);
void damon_perf_probe_teardown(struct damon_ctx *ctx,
			       struct damon_perf_probe_event *event);

#endif /* CONFIG_DAMON_PERF_SOURCE */
#endif /* _DAMON_PERF_SOURCE_H */
