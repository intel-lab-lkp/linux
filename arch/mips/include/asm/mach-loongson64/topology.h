/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_MACH_TOPOLOGY_H
#define _ASM_MACH_TOPOLOGY_H

#include <asm/numa.h>

#ifdef CONFIG_NUMA
#define early_cpu_to_node(cpu)	(cpu_logical_map(cpu) >> 2)
#define cpu_to_node(cpu)  early_cpu_to_node(cpu)

extern cpumask_t __node_cpumask[];
#define cpumask_of_node(node)	(&__node_cpumask[node])

#endif

#include <asm-generic/topology.h>

#endif /* _ASM_MACH_TOPOLOGY_H */
