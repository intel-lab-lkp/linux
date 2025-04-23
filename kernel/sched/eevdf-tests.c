// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc
 *
 * Author: Dhaval Giani (AMD) <dhaval@gianis.ca>
 *
 * Basic functional tests for EEVDF - Invariants
 *
 * Use the debugfs triggers to run them
 *
 */

#include <linux/debugfs.h>
#include <linux/sched.h>

#include "sched.h"

#ifdef CONFIG_SCHED_EEVDF_TESTING

/*
 * Test parameters
 */
bool eevdf_positive_lag_test;
u8 eevdf_positive_lag_count = 10;

static struct dentry *debugfs_eevdf_testing;
void debugfs_eevdf_testing_init(struct dentry *debugfs_sched)
{
	debugfs_eevdf_testing = debugfs_create_dir("eevdf-testing", debugfs_sched);

	debugfs_create_bool("eevdf_positive_lag_test", 0700,
				debugfs_eevdf_testing, &eevdf_positive_lag_test);
	debugfs_create_u8("eevdf_positive_lag_test_count", 0600,
				debugfs_eevdf_testing, &eevdf_positive_lag_count);

}

void test_eevdf_positive_lag(struct cfs_rq *cfs, struct sched_entity *se)
{
	static int eevdf_positive_lag_test_counter;
	u64 eevdf_average_vruntime;

	if (!eevdf_positive_lag_test)
		return;

	if (!se || !cfs)
		return;

	eevdf_average_vruntime = avg_vruntime(cfs);
	eevdf_positive_lag_test_counter++;

	if (se->vruntime > eevdf_average_vruntime) {
		trace_printk("FAIL: Lemma 1 failed - selected task has negative lag\n");
		eevdf_positive_lag_test = 0;
		eevdf_positive_lag_test_counter = 0;
		return;
	}

	if (eevdf_positive_lag_test_counter > eevdf_positive_lag_count) {
		eevdf_positive_lag_test = 0;
		eevdf_positive_lag_test_counter = 0;
		trace_printk("PASS: At least %u selected tasks had positive lag\n",
							eevdf_positive_lag_count);
	}
}

#endif /* CONFIG_SCHED_EEVDF_TESTING */
