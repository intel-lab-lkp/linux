// SPDX-License-Identifier: GPL-2.0
/*
 * Memory Bandwidth Allocation (MBA) test
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include "resctrl.h"

#define RESULT_FILE_NAME	"result_mba"
#define NUM_OF_RUNS		5
#define MAX_DIFF_PERCENT	15

/* Intel MBA is specified as a percentage: 10%, 20% ... 100%. */
#define ALLOCATION_MAX		100
#define ALLOCATION_MIN		10
#define ALLOCATION_STEP		10

/*
 * AMD MBA is not a percentage but an absolute memory bandwidth value. The
 * test sweeps the bandwidth from 4 GB/s to 40 GB/s in steps of 4 GB/s,
 * which the hardware represents in units of 1/8 GB/s (i.e. 4 GB/s -> 32,
 * 8 GB/s -> 64, ... 40 GB/s -> 320).
 */
#define AMD_ALLOCATION_MAX	320
#define AMD_ALLOCATION_MIN	32
#define AMD_ALLOCATION_STEP	32

/* Number of MBA schemata allocations to sweep (same for both vendors). */
#define NUM_OF_ALLOCS		(ALLOCATION_MAX / ALLOCATION_STEP)

/*
 * The fixed-size result arrays in check_results() and the aggregation loop in
 * show_mba_info() are sized and driven by NUM_OF_ALLOCS, so each vendor's sweep
 * must produce exactly NUM_OF_ALLOCS allocations. Enforce this at build time so
 * that changing a vendor's min/max/step can never overflow those arrays.
 */
_Static_assert((ALLOCATION_MAX - ALLOCATION_MIN) / ALLOCATION_STEP + 1 == NUM_OF_ALLOCS,
	       "Intel MBA sweep must yield NUM_OF_ALLOCS allocations");
_Static_assert((AMD_ALLOCATION_MAX - AMD_ALLOCATION_MIN) / AMD_ALLOCATION_STEP + 1 == NUM_OF_ALLOCS,
	       "AMD MBA sweep must yield NUM_OF_ALLOCS allocations");

static unsigned int mba_alloc_min(void)
{
	return get_vendor() == ARCH_AMD ? AMD_ALLOCATION_MIN : ALLOCATION_MIN;
}

static unsigned int mba_alloc_max(void)
{
	return get_vendor() == ARCH_AMD ? AMD_ALLOCATION_MAX : ALLOCATION_MAX;
}

static unsigned int mba_alloc_step(void)
{
	return get_vendor() == ARCH_AMD ? AMD_ALLOCATION_STEP : ALLOCATION_STEP;
}

static int mba_init(const struct resctrl_test *test,
		    const struct user_params *uparams,
		    const struct resctrl_val_param *param, int domain_id)
{
	int ret;

	ret = initialize_read_mem_bw_mc();
	if (ret)
		return ret;

	initialize_mem_bw_resctrl(param, domain_id);

	return 0;
}

/*
 * Sweep the MBA schemata from the minimum to the maximum allocation. On
 * Intel the allocation is a percentage (10% .. 100%); on AMD it is an
 * absolute memory bandwidth value (4 GB/s .. 40 GB/s). Write schemata to
 * the specified con_mon grp, mon_grp in resctrl FS.
 * For each allocation, run 5 times in order to get average values.
 */
static int mba_setup(const struct resctrl_test *test,
		     const struct user_params *uparams,
		     struct resctrl_val_param *p)
{
	static unsigned int allocation;
	static int runs_per_allocation;
	char allocation_str[64];
	int ret;

	if (runs_per_allocation >= NUM_OF_RUNS)
		runs_per_allocation = 0;

	/* Only set up schemata once every NUM_OF_RUNS of allocations */
	if (runs_per_allocation++ != 0)
		return 0;

	/* Set the initial allocation once the vendor is known. */
	if (allocation == 0)
		allocation = mba_alloc_min();

	if (allocation > mba_alloc_max())
		return END_OF_TESTS;

	sprintf(allocation_str, "%u", allocation);

	ret = write_schemata(p->ctrlgrp, allocation_str, uparams->cpu, test->resource);
	if (ret < 0)
		return ret;

	allocation += mba_alloc_step();

	return 0;
}

static int mba_measure(const struct user_params *uparams,
		       struct resctrl_val_param *param, pid_t bm_pid)
{
	return measure_read_mem_bw(uparams, param, bm_pid);
}

static bool show_mba_info(unsigned long *bw_mc, unsigned long *bw_resc)
{
	unsigned int alloc_min = mba_alloc_min();
	unsigned int alloc_step = mba_alloc_step();
	unsigned int allocation;
	bool ret = false;
	int runs;

	ksft_print_msg("Results are displayed in (MB)\n");
	/* Memory bandwidth for each of the swept MBA allocations */
	for (allocation = 0; allocation < NUM_OF_ALLOCS; allocation++) {
		unsigned long sum_bw_mc = 0, sum_bw_resc = 0;
		long avg_bw_mc, avg_bw_resc;
		int avg_diff_per;
		float avg_diff;

		for (runs = NUM_OF_RUNS * allocation;
		     runs < NUM_OF_RUNS * allocation + NUM_OF_RUNS ; runs++) {
			sum_bw_mc += bw_mc[runs];
			sum_bw_resc += bw_resc[runs];
		}

		avg_bw_mc = sum_bw_mc / NUM_OF_RUNS;
		avg_bw_resc = sum_bw_resc / NUM_OF_RUNS;
		if (avg_bw_mc < THROTTLE_THRESHOLD || avg_bw_resc < THROTTLE_THRESHOLD) {
			ksft_print_msg("Bandwidth below threshold (%d MiB). Dropping results from MBA schemata %u.\n",
				       THROTTLE_THRESHOLD,
				       alloc_min + alloc_step * allocation);
			continue;
		}

		avg_diff = (float)labs(avg_bw_resc - avg_bw_mc) / avg_bw_mc;
		avg_diff_per = (int)(avg_diff * 100);

		ksft_print_msg("%s Check MBA diff within %d%% for schemata %u\n",
			       avg_diff_per > MAX_DIFF_PERCENT ?
			       "Fail:" : "Pass:",
			       MAX_DIFF_PERCENT,
			       alloc_min + alloc_step * allocation);

		ksft_print_msg("avg_diff_per: %d%%\n", avg_diff_per);
		ksft_print_msg("avg_bw_mc: %lu\n", avg_bw_mc);
		ksft_print_msg("avg_bw_resc: %lu\n", avg_bw_resc);
		if (avg_diff_per > MAX_DIFF_PERCENT)
			ret = true;
	}

	ksft_print_msg("%s Check schemata change using MBA\n",
		       ret ? "Fail:" : "Pass:");
	if (ret)
		ksft_print_msg("At least one test failed\n");

	return ret;
}

static int check_results(void)
{
	unsigned long bw_resc[NUM_OF_RUNS * NUM_OF_ALLOCS];
	unsigned long bw_mc[NUM_OF_RUNS * NUM_OF_ALLOCS];
	char *token_array[8], output[] = RESULT_FILE_NAME, temp[512];
	int runs;
	FILE *fp;

	fp = fopen(output, "r");
	if (!fp) {
		ksft_perror(output);

		return -1;
	}

	runs = 0;
	while (fgets(temp, sizeof(temp), fp)) {
		char *token = strtok(temp, ":\t");
		int fields = 0;

		if (runs >= NUM_OF_RUNS * NUM_OF_ALLOCS) {
			ksft_print_msg("Got more results than expected\n");
			break;
		}

		while (token) {
			token_array[fields++] = token;
			token = strtok(NULL, ":\t");
		}

		/* Field 3 is perf mc value */
		bw_mc[runs] = strtoul(token_array[3], NULL, 0);
		/* Field 5 is resctrl value */
		bw_resc[runs] = strtoul(token_array[5], NULL, 0);
		runs++;
	}

	fclose(fp);

	return show_mba_info(bw_mc, bw_resc);
}

static void mba_test_cleanup(void)
{
	remove(RESULT_FILE_NAME);
}

static int mba_run_test(const struct resctrl_test *test, const struct user_params *uparams)
{
	struct resctrl_val_param param = {
		.ctrlgrp	= "c1",
		.filename	= RESULT_FILE_NAME,
		.init		= mba_init,
		.setup		= mba_setup,
		.measure	= mba_measure,
	};
	struct fill_buf_param fill_buf = {};
	int ret;

	remove(RESULT_FILE_NAME);

	if (uparams->fill_buf) {
		fill_buf.buf_size = uparams->fill_buf->buf_size;
		fill_buf.memflush = uparams->fill_buf->memflush;
		param.fill_buf = &fill_buf;
	} else if (!uparams->benchmark_cmd[0]) {
		ssize_t buf_size;

		buf_size = get_fill_buf_size(uparams->cpu, "L3");
		if (buf_size < 0)
			return buf_size;
		fill_buf.buf_size = buf_size;
		fill_buf.memflush = true;
		param.fill_buf = &fill_buf;
	}

	ret = resctrl_val(test, uparams, &param);
	if (ret)
		return ret;

	ret = check_results();
	if (ret && (get_vendor() == ARCH_INTEL) && !snc_kernel_support())
		ksft_print_msg("Kernel doesn't support Sub-NUMA Clustering but it is enabled on the system.\n");

	return ret;
}

static bool mba_feature_check(const struct resctrl_test *test)
{
	return test_resource_feature_check(test) &&
	       resctrl_mon_feature_exists("L3_MON", "mbm_local_bytes");
}

struct resctrl_test mba_test = {
	.name = "MBA",
	.resource = "MB",
	.vendor_specific = ARCH_INTEL | ARCH_AMD,
	.feature_check = mba_feature_check,
	.run_test = mba_run_test,
	.cleanup = mba_test_cleanup,
};
