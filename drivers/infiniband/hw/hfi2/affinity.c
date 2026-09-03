// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2015 - 2020 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include <linux/topology.h>
#include <linux/cpumask.h>
#include <linux/numa.h>

#include "hfi2.h"
#include "affinity.h"

/*
 * Build a per-device completion vector to CPU mapping table.
 *
 * The driver advertises a number of completion vectors equal to the count
 * of CPUs on the device's NUMA node (capped at the number of online CPUs).
 * For each vector, a CPU is chosen by spreading completion-vector slots
 * across the NUMA-local CPUs.  rdmavt looks up the mapping via
 * hfi2_comp_vect_mappings_lookup() and uses it to schedule per-CQ work
 * with queue_work_on().
 */

static void _dev_comp_vect_mappings_destroy(struct hfi2_devdata *dd)
{
	kfree(dd->comp_vect_mappings);
	dd->comp_vect_mappings = NULL;
	dd->comp_vect_possible_cpus = 0;
}

int hfi2_comp_vectors_set_up(struct hfi2_devdata *dd)
{
	unsigned int possible;
	int i;

	/*
	 * Use the count of CPUs on the device's NUMA node.  If the node has
	 * no CPUs, fall back to the number of online CPUs.
	 */
	possible = cpumask_weight(cpumask_of_node(dd->node));
	if (!possible)
		possible = num_online_cpus();
	if (!possible)
		return -EINVAL;

	dd->comp_vect_mappings =
		kcalloc(possible, sizeof(*dd->comp_vect_mappings), GFP_KERNEL);
	if (!dd->comp_vect_mappings)
		return -ENOMEM;

	dd->comp_vect_possible_cpus = possible;
	for (i = 0; i < possible; i++)
		dd->comp_vect_mappings[i] = cpumask_local_spread(i, dd->node);

	return 0;
}

void hfi2_comp_vectors_clean_up(struct hfi2_devdata *dd)
{
	_dev_comp_vect_mappings_destroy(dd);
}

int hfi2_comp_vect_mappings_lookup(struct rvt_dev_info *rdi, int comp_vect)
{
	struct hfi2_ibdev *verbs_dev = dev_from_rdi(rdi);
	struct hfi2_devdata *dd = dd_from_dev(verbs_dev);

	if (!dd->comp_vect_mappings)
		return -EINVAL;
	if (comp_vect >= dd->comp_vect_possible_cpus)
		return -EINVAL;

	return dd->comp_vect_mappings[comp_vect];
}
