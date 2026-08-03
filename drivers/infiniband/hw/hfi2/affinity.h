/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2015 - 2020 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _HFI2_AFFINITY_H
#define _HFI2_AFFINITY_H

#include "hfi2.h"

/*
 * Completion vector to CPU mapping.
 *
 * The driver populates a per-device table that maps each completion vector
 * to a CPU on the device's NUMA node.  rdmavt consults this table via
 * hfi2_comp_vect_mappings_lookup() when scheduling CQ work on a specific
 * CPU.  This is kernel-internal infrastructure, not user-visible policy.
 * CPU affinity for user processes and IRQs is left to user space
 * (sched_setaffinity, irqbalance, /proc/irq/<n>/smp_affinity).
 */
int hfi2_comp_vect_mappings_lookup(struct rvt_dev_info *rdi, int comp_vect);
int hfi2_comp_vectors_set_up(struct hfi2_devdata *dd);
void hfi2_comp_vectors_clean_up(struct hfi2_devdata *dd);

#endif /* _HFI2_AFFINITY_H */
