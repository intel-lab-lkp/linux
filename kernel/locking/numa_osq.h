/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_NUMA_OSQ_H
#define __LINUX_NUMA_OSQ_H

#include <linux/osq_lock.h>
#include "mcs_spinlock.h"

#define OSQLOCKINITED 0
#define OSQTONUMADETECT 0x10
#define OSQLOCKSTOPPING 0xfc
#define OSQ_LOCKED_VAL 0xffff

extern u16 osq_keep_times;
extern u16 osq_lock_depth;
extern int osq_node_max;

inline int encode_cpu(int cpu_nr);
inline int node_cpu(struct optimistic_spin_node *node);
inline struct optimistic_spin_node *decode_cpu(int encoded_cpu_val);

void zx_osq_lock_stopping(struct optimistic_spin_queue *lock);
void zx_osq_numa_start(struct optimistic_spin_queue *lock);
void zx_osq_turn_numa_waiting(struct optimistic_spin_queue *lock);

bool x_osq_lock(struct optimistic_spin_queue *lock);
void x_osq_unlock(struct optimistic_spin_queue *lock);
bool x_osq_is_locked(struct optimistic_spin_queue *lock);
inline void zx_numa_osq_unlock(struct optimistic_spin_queue *qslock,
		struct _numa_lock *n);
inline bool zx_numa_osq_lock(struct optimistic_spin_queue *qslock,
		struct _numa_lock *n);
#endif
