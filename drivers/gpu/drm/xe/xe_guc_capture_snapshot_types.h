/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021-2024 Intel Corporation
 */

#ifndef _XE_GUC_CAPTURE_SNAPSHOT_TYPES_H
#define _XE_GUC_CAPTURE_SNAPSHOT_TYPES_H

#include <linux/types.h>
#include <abi/guc_capture_abi.h>
#include "xe_guc_fwif.h"

struct drm_printer;
struct guc_mmio_reg;
struct xe_guc;
struct xe_exec_queue;
struct xe_hw_engine;

enum xe_guc_capture_snapshot_source {
	XE_ENGINE_CAPTURE_SOURCE_MANUAL,
	XE_ENGINE_CAPTURE_SOURCE_GUC
};

/*
 * struct xe_guc_capture_snapshot - extracted error capture node
 *
 * A single unit of extracted error-capture output data grouped together
 * at an engine-instance level. We keep these nodes in a linked list.
 * See cachelist and outlist below.
 */
struct xe_guc_capture_snapshot {
	/*
	 * A single set of 3 capture lists: a global-list
	 * an engine-class-list and an engine-instance list.
	 * outlist in __guc_capture_parsed_output will keep
	 * a linked list of these nodes that will eventually
	 * be detached from outlist and attached into to
	 * xe_codedump in response to a context reset
	 */
	struct list_head link;
	bool is_partial;
	u32 eng_class;
	u32 eng_inst;
	u32 guc_id;
	u32 lrca;
	u32 type;
	bool locked;
	enum xe_guc_capture_snapshot_source source;
	struct gcap_reg_list_info {
		u32 vfid;
		u32 num_regs;
		struct guc_mmio_reg *regs;
	} reginfo[GUC_STATE_CAPTURE_TYPE_MAX];
#define GCAP_PARSED_REGLIST_INDEX_GLOBAL   BIT(GUC_STATE_CAPTURE_TYPE_GLOBAL)
#define GCAP_PARSED_REGLIST_INDEX_ENGCLASS BIT(GUC_STATE_CAPTURE_TYPE_ENGINE_CLASS)
};

struct xe_guc_capture_snapshot *
xe_guc_capture_snapshot_get(struct xe_guc *guc, struct xe_exec_queue *q,
			    enum xe_guc_capture_snapshot_source src);
void xe_guc_capture_snapshot_print(struct xe_guc *guc, struct xe_guc_capture_snapshot *node,
				   struct drm_printer *p);
void xe_guc_capture_snapshot_put(struct xe_guc *guc, struct xe_guc_capture_snapshot *snapshot);
void xe_guc_capture_snapshot_store_manual_job(struct xe_guc *guc, struct xe_exec_queue *q);
struct xe_guc_capture_snapshot *
xe_guc_capture_snapshot_store_and_get_manual_hwe(struct xe_guc *guc, struct xe_hw_engine *hwe);

#endif
