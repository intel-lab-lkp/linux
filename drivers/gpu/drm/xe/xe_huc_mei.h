/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Arisa Snowbell
 */

#ifndef _XE_HUC_MEI_H_
#define _XE_HUC_MEI_H_

#include <linux/types.h>

struct dma_fence;
struct xe_huc;

bool xe_huc_mei_is_used(struct xe_huc *huc);
void xe_huc_mei_init(struct xe_huc *huc);
void xe_huc_mei_start(struct xe_huc *huc);
void xe_huc_mei_stop(struct xe_huc *huc);
struct dma_fence *xe_huc_mei_load_fence(struct xe_huc *huc);
int xe_huc_mei_wait(struct xe_huc *huc);

#endif
