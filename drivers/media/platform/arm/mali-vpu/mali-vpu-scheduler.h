/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_SCHEDULER_H__
#define __MALI_VPU_SCHEDULER_H__

#include <linux/mutex.h>

#include "mali-vpu-lsid.h"

struct mali_vpu_dev;
struct mali_vpu_session;

/**
 * struct mali_vpu_sched_session - Client session class.
 * @lsid: Pointer to LSID the session is mapped to.
 * @pcb: LSID process control block programmed when the session is mapped.
 * @core_count: Number of cores assigned to this scheduled session.
 *
 * This struct is used to keep track of session's information needed by the scheduler.
 */
struct mali_vpu_sched_session {
	struct mali_vpu_lsid *lsid;
	struct mali_vpu_lsid_pcb pcb;
	u32 core_count;
};

/**
 * struct mali_vpu_sched - Scheduler class.
 * @vpu_dev: Pointer to mali_vpu_dev structure.
 * @lock: Serialise access to sched resources.
 * @lsid_list: Array of LSID instances.
 * @nlsid: Number of LSIDs.
 */
struct mali_vpu_sched {
	struct mali_vpu_dev *vpu_dev;
	struct mutex lock;	/* Serialise access to sched resources. */
	struct mali_vpu_lsid *lsid_list;
	u32 nlsid;
};

int mali_vpu_sched_init(struct mali_vpu_sched *sched, struct mali_vpu_dev *dev);
void mali_vpu_sched_cleanup(struct mali_vpu_sched *sched);
int mali_vpu_sched_session_prepare(struct mali_vpu_sched_session *sched_sess,
				   struct mali_vpu_session *sess);
int mali_vpu_sched_switch_in(struct mali_vpu_sched_session *sched_sess,
			     struct mali_vpu_sched *sched);
void mali_vpu_sched_send_irq(struct mali_vpu_sched_session *sched_sess,
			     struct mali_vpu_sched *sched);
void mali_vpu_sched_flush_mmu(struct mali_vpu_sched_session *sched_sess,
			      struct mali_vpu_sched *sched);
int mali_vpu_sched_session_terminate(struct mali_vpu_sched_session *sched_sess,
				     struct mali_vpu_sched *sched);

#endif /* __MALI_VPU_SCHEDULER_H__ */
