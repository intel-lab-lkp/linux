/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_BUILD_ID_TOOL_H
#define __PERF_BUILD_ID_TOOL_H

#include "tool.h"

struct strlist;
struct evsel;

enum build_id_rewrite_style {
	BID_RWS__NONE = 0,
	BID_RWS__INJECT_HEADER_LAZY,
	BID_RWS__INJECT_HEADER_ALL,
	BID_RWS__MMAP2_BUILDID_ALL,
	BID_RWS__MMAP2_BUILDID_LAZY,
};

struct build_id_tool {
	struct delegate_tool dtool;
	enum build_id_rewrite_style style;
	struct strlist *known_build_ids;
	const struct evsel *mmap_evsel;
};

struct build_id_tool *build_id_tool__new(enum build_id_rewrite_style style,
					 const char *known_build_ids_string,
					 struct perf_tool *delegate);

void build_id_tool__delete(struct build_id_tool *bit);

#endif /* __PERF_BUILD_ID_TOOL_H */
