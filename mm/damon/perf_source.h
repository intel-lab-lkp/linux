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

/*
 * PMU event attributes for a perf-event probe.  A subset of perf_event_attr
 * chosen at probe creation time to select the PMU (AMD IBS, Intel PEBS, ...)
 * and its sampling parameters.
 */
struct damon_perf_event_attr {
	u32 type;
	u64 config;
	u64 config1;
	u64 config2;
	bool sample_phys_addr;
	bool sample_weight_struct;
	bool exclude_kernel;
	bool exclude_hv;
	bool freq;
	/* system-wide PMU: open one counter, not one per CPU */
	bool single_instance;
	u64 sample_freq;
	u64 sample_period;
	u32 wakeup_events;
	u32 precise_ip;
};

struct damon_perf_probe_event {
	struct damon_perf_event_attr attr;
	struct damon_ctx *ctx;	/* owning ctx for ring routing; set at setup */
	void *priv;		/* struct damon_perf_probe_state * */
	struct hlist_node hlist_node;
	/* 1-based probe index for probe_hits[]; set at registration */
	int probe_idx;
};

int damon_perf_probe_setup(struct damon_ctx *ctx,
			   struct damon_probe *probe,
			   struct damon_perf_probe_event *event);
void damon_perf_probe_teardown(struct damon_ctx *ctx,
			       struct damon_perf_probe_event *event);

#endif /* CONFIG_DAMON_PERF_SOURCE */
#endif /* _DAMON_PERF_SOURCE_H */
