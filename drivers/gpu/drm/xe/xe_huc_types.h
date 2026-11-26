/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_HUC_TYPES_H_
#define _XE_HUC_TYPES_H_

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "xe_uc_fw_types.h"

struct dma_fence;
struct i915_pxp_component;
struct xe_bo;

/**
 * enum xe_huc_mei_state - where a HuC that the GSC loads through mei is
 * @XE_HUC_MEI_UNUSED: not loaded through mei (not DG2, or its setup failed)
 * @XE_HUC_MEI_DOWN: the GT is not running (before the first load, in a reset or
 *	a suspend), there is nothing to load into
 * @XE_HUC_MEI_LOADING: the GT runs and the GSC load is pending
 * @XE_HUC_MEI_SETTLED: the load succeeded, failed or stopped waiting for mei
 */
enum xe_huc_mei_state {
	XE_HUC_MEI_UNUSED,
	XE_HUC_MEI_DOWN,
	XE_HUC_MEI_LOADING,
	XE_HUC_MEI_SETTLED,
};

/**
 * struct xe_huc_mei - HuC loaded by the GSC through mei_pxp (DG2)
 */
struct xe_huc_mei {
	/** @lock: protects @pxp, held across a GSC command */
	struct mutex lock;
	/** @pxp: the bound mei PXP component, NULL while unbound */
	struct i915_pxp_component *pxp;
	/** @msg: VRAM page for the GSC request and reply */
	struct xe_bo *msg;
	/** @auth: asks the GSC to load and authenticate the HuC */
	struct work_struct auth;
	/** @deadline: stops waiting for a mei that does not bind */
	struct delayed_work deadline;
	/** @state_lock: protects @state, @bound, @absent and @load */
	spinlock_t state_lock;
	/** @state: where the load is */
	enum xe_huc_mei_state state;
	/** @bound: the mei PXP component is bound */
	bool bound;
	/** @absent: mei did not bind in time, later loads don't wait for it */
	bool absent;
	/** @announced: the first successful load was logged */
	bool announced;
	/** @load: signalled when the pending load settles, NULL when none */
	struct dma_fence *load;
	/** @fence_context: dma-fence context of @load */
	u64 fence_context;
	/** @seqno: sequence number of the last @load */
	atomic64_t seqno;
};

/**
 * struct xe_huc - HuC
 */
struct xe_huc {
	/** @fw: Generic uC firmware management */
	struct xe_uc_fw fw;

	/** @gsc_pkt: bo to store the packet for auth via GSC */
	struct xe_bo *gsc_pkt;

	/** @mei: loading by the GSC through mei (DG2) */
	struct xe_huc_mei mei;
};

#endif
