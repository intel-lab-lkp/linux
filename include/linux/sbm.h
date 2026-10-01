/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SBM_H
#define _LINUX_SBM_H

int arch_sbm_cpu_instance_id(int cpu);
void sbm_set_topology(int num_instances, int max_threads_per_instance);

#endif /* _LINUX_SBM_H */
