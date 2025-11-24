/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __DATA_CONVERT_H
#define __DATA_CONVERT_H

#include <stdbool.h>
#include <linux/types.h>

struct perf_data_convert_opts {
	bool force;
	bool all;
	bool tod;
	u64 range_start;
	u64 range_end;
};

#ifdef HAVE_LIBBABELTRACE_SUPPORT
int bt_convert__perf2ctf(const char *input_name, const char *to_ctf,
			 struct perf_data_convert_opts *opts);
#endif /* HAVE_LIBBABELTRACE_SUPPORT */

int bt_convert__perf2json(const char *input_name, const char *to_ctf,
			 struct perf_data_convert_opts *opts);

#endif /* __DATA_CONVERT_H */
