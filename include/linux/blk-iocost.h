/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BLK_IOCOST_H
#define _LINUX_BLK_IOCOST_H

#include <linux/types.h>

#ifdef CONFIG_BLK_CGROUP_IOCOST_BPF

/*
 * Pluggable cost model interface for blk-iocost.
 *
 * A BPF struct_ops implementation registered against "iocost_model_ops"
 * replaces the builtin linear cost model on devices configured with
 * "ctrl=bpf" through io.cost.model.  The model is a pure function of the
 * arguments below and returns the cost of the IO in device time units,
 * where 1 second of device time equals 2^37 (VTIME_PER_SEC).  The
 * returned value is clamped by the kernel.  A cost of 0 delegates the
 * IO back to the builtin formula, so a model which does not handle a
 * given IO type cannot make it free.
 *
 * The iocg_id argument identifies the issuing cgroup (css id) so the
 * model can keep per-cgroup state, e.g. for multi-stream sequentiality
 * detection.  The id is only valid while the cgroup exists: css ids are
 * recycled once the cgroup is removed, so models must treat it as a
 * transient key and reset state when they observe it reused.  Whether
 * the interface needs a release(iocg_id) callback for state cleanup is
 * an open question.
 *
 * The model is called from the IO submission path under RCU and must
 * not sleep.  Only the bio-level charging path consults the model;
 * the request-level sizing path (calc_size_vtime_cost()) keeps using
 * the builtin formula.
 */

/* flags for calc_cost() */
#define IOCOST_COST_F_MERGE	(1ULL << 0)	/* called from merge path */

struct iocost_model_ops {
	/*
	 * @op:		REQ_OP_* value (uapi blk_opf.h)
	 * @nbytes:	IO size in bytes
	 * @sector:	starting sector
	 * @cursor:	iocg cursor sector, 0 if none (sequentiality hint)
	 * @iocg_id:	css id of the issuing cgroup
	 * @flags:	IOCOST_COST_F_*
	 */
	u64 (*calc_cost)(u64 op, u64 nbytes, u64 sector, u64 cursor,
			 u64 iocg_id, u64 flags);
};

bool iocost_bpf_calc_cost(u64 op, u64 nbytes, u64 sector, u64 cursor,
			  u64 iocg_id, u64 flags, u64 *costp);
bool iocost_bpf_model_registered(void);

#endif	/* CONFIG_BLK_CGROUP_IOCOST_BPF */
#endif	/* _LINUX_BLK_IOCOST_H */
