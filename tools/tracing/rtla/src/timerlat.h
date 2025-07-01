// SPDX-License-Identifier: GPL-2.0
#include "osnoise.h"

struct timerlat_params {
	struct common_params	common;
	long long		timerlat_period_us;
	long long		print_stack;
	int			output_divisor;
	int			dma_latency;
	int			no_aa;
	int			dump_tasks;
	int			user_workload;
	int			kernel_workload;
	int			user_data;
	int			deepest_idle_state;
	union {
		struct {
			/* top only */
			int			quiet;
			int			aa_only;
			int			pretty_output;
		};
		struct {
			/* hist only */
			char			no_irq;
			char			no_thread;
			char			no_header;
			char			no_summary;
			char			no_index;
			char			with_zeros;
			int			bucket_size;
			int			entries;
		};
	};
};

int timerlat_apply_config(struct osnoise_tool *tool, struct timerlat_params *params);

int timerlat_hist_main(int argc, char *argv[]);
int timerlat_top_main(int argc, char *argv[]);
int timerlat_main(int argc, char *argv[]);
