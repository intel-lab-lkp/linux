// SPDX-License-Identifier: GPL-2.0
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/node.h>
#include <linux/nodemask.h>
#include <linux/percpu.h>
#include <linux/sbm.h>

static DEFINE_PER_CPU(struct cpu, cpu_devices);

#ifdef CONFIG_NUMA
/* sbm setup data - only needed during init */
static struct cpumask cpu_sbm_setup_map __initdata;
static int __node_thread_count[NR_CPUS] __initdata;

int arch_sbm_cpu_instance_id(int cpu)
{
	return cpu_to_node(cpu);
}

void __init configure_sbm_topology(void)
{
	int num_sbm_instances, max_threads_per_instance = 1;
	int i;

	for_each_possible_cpu(i) {
		bool found = false;
		int j;

		for_each_cpu(j, &cpu_sbm_setup_map) {
			if (cpu_to_node(i) == cpu_to_node(j)) {
				found = true;
				break;
			}
		}

		if (!found) {
			cpumask_set_cpu(i, &cpu_sbm_setup_map);
			__node_thread_count[i] = 1;
			continue;
		}

		__node_thread_count[j] += 1;
		max_threads_per_instance = max(max_threads_per_instance,
					       __node_thread_count[j]);
	}

	num_sbm_instances = cpumask_weight(&cpu_sbm_setup_map);
	sbm_set_topology(num_sbm_instances, max_threads_per_instance);
}
#endif /* CONFIG_NUMA */

static int __init topology_init(void)
{
	int i, ret;

	for_each_present_cpu(i) {
		struct cpu *c = &per_cpu(cpu_devices, i);

		c->hotpluggable = !!i;
		ret = register_cpu(c, i);
		if (ret)
			printk(KERN_WARNING "topology_init: register_cpu %d "
			       "failed (%d)\n", i, ret);
	}

	return 0;
}

subsys_initcall(topology_init);
