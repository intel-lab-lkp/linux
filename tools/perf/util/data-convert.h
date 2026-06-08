/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __DATA_CONVERT_H
#define __DATA_CONVERT_H

#include <stdbool.h>

struct perf_data_convert_opts {
	bool force;
	bool all;
	bool tod;
	const char *time_str;
};

#ifdef HAVE_LIBBABELTRACE_SUPPORT
int bt_convert__perf2ctf(const char *input_name, const char *to_ctf,
			 struct perf_data_convert_opts *opts);
#endif /* HAVE_LIBBABELTRACE_SUPPORT */

int bt_convert__perf2json(const char *input_name, const char *to_ctf,
			 struct perf_data_convert_opts *opts);

#ifdef HAVE_LIBTRACEEVENT
int trace_convert__perf2dat(const char *input, const char *to_trace,
			   struct perf_data_convert_opts *opts);
#endif /* HAVE_LIBTRACEEVENT */
#endif /* __DATA_CONVERT_H */
