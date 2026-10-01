/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SBM_H
#define _LINUX_SBM_H

/*
 * Masks and shifts for sbm index to translate
 * a sbm leaf to CPU.
 */
extern int __sbm_shift;
extern int __sbm_mask;

int arch_sbm_cpu_instance_id(int cpu);
void sbm_set_topology(int num_instances, int max_threads_per_instance);

int sbm_cpu_to_idx(int cpu);
int sbm_idx_to_cpu(int idx);

int alloc_sbm_index(int cpu);
void free_sbm_index(int cpu);
int sbm_init(void);

#endif /* _LINUX_SBM_H */
