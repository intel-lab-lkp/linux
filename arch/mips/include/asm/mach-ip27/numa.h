/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Jiaxun Yang <jiaxun.yang@flygoat.com>
 */

#ifndef __ASM_MACH_NUMA_H
#define __ASM_MACH_NUMA_H

#ifdef CONFIG_NUMA
extern unsigned char __node_distances[MAX_NUMNODES][MAX_NUMNODES];

#define node_distance(from, to) (__node_distances[(from)][(to)])
#endif

/* Hanlded in platform code */
static inline void numa_store_cpu_info(unsigned int cpu) { }
static inline void numa_add_cpu(unsigned int cpu) { }
static inline void numa_remove_cpu(unsigned int cpu) { }
static inline void arch_numa_init(void) { }
static inline void early_map_cpu_to_node(unsigned int cpu, int nid) { }

#endif	/* __ASM_NUMA_H */
