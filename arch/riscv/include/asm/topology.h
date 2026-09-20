/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_TOPOLOGY_H
#define _ASM_RISCV_TOPOLOGY_H

#include <linux/arch_topology.h>

#ifdef CONFIG_NUMA
#include <asm/numa.h>
#endif

#define topology_die_id(cpu)		(cpu_topology[cpu].die_id)
#define topology_die_cpumask(cpu)	(&cpu_topology[cpu].die_sibling)
#define topology_die_sysfs_visible(cpu)	(topology_die_id(cpu) >= 0)

/* Replace task scheduler's default frequency-invariant accounting */
#define arch_scale_freq_tick		topology_scale_freq_tick
#define arch_set_freq_scale		topology_set_freq_scale
#define arch_scale_freq_capacity	topology_get_freq_scale
#define arch_scale_freq_invariant	topology_scale_freq_invariant
#define arch_scale_freq_ref		topology_get_freq_ref

/* Replace task scheduler's default cpu-invariant accounting */
#define arch_scale_cpu_capacity	topology_get_cpu_scale

/* Enable topology flag updates */
#define arch_update_cpu_topology	topology_update_cpu_topology

#include <asm-generic/topology.h>

#endif /* _ASM_RISCV_TOPOLOGY_H */
