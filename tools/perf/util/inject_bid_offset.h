/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_INJECT_BID_OFFSET_H
#define __PERF_INJECT_BID_OFFSET_H

#include <linux/perf_event.h>

struct perf_tool;
struct evlist;

int perf_event__rewrite_attr_for_build_id_offset(struct perf_event_attr *attr);
int perf_event__rewrite_attr_for_sample_ip(struct perf_event_attr *attr);
struct perf_tool *inject_bid_offset_tool__new(struct perf_tool *delegate);
void inject_bid_offset_tool__delete(struct perf_tool *tool);

#endif /* __PERF_INJECT_BID_OFFSET_H */
