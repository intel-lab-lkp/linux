/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_MACH_TOPOLOGY_H
#define _ASM_MACH_TOPOLOGY_H	1

#include <asm/sn/types.h>
#include <asm/mmzone.h>
#include <asm/numa.h>

struct cpuinfo_ip27 {
	nasid_t		p_nasid;	/* my node ID in numa-as-id-space */
	unsigned short	p_speed;	/* cpu speed in MHz */
	unsigned char	p_slice;	/* Physical position on node board */
};

extern struct cpuinfo_ip27 sn_cpu_info[NR_CPUS];

#define early_cpu_to_node(cpu)	(cputonasid(cpu))
#define cpu_to_node(cpu)  early_cpu_to_node(cpu)
#define cpumask_of_node(node)	((node) == -1 ?				\
				 cpu_all_mask :				\
				 &hub_data(node)->h_cpus)

#include <asm-generic/topology.h>

#endif /* _ASM_MACH_TOPOLOGY_H */
