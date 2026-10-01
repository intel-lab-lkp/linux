/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/sbm.h>
#include <linux/init.h>
#include <linux/printk.h>

static int sbm_max_threads_per_instance = -1;
static int sbm_num_instance = -1;

/*
 * In absence of an arch definition, consider all CPUs to
 * belong to the same sbm instance.
 */
int __weak arch_sbm_cpu_instance_id(int cpu)
{
	return 0;
}

void __init sbm_set_topology(int num_instances, int max_threads_per_instance)
{
	sbm_max_threads_per_instance = max_threads_per_instance;
	sbm_num_instance = num_instances;

	pr_info("sbm topology set to %d instance(s) with a maximum of %d thread(s) per instance\n",
		sbm_num_instance,
		sbm_max_threads_per_instance);
}

