/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright 2025 NXP
 */

#ifndef __DCIF_CRC_H__
#define __DCIF_CRC_H__

#include <linux/types.h>

#include "dcif-drv.h"

static inline bool enable_dcif_crc_needed(struct dcif_crtc_state *new_dcstate,
					  struct dcif_crtc_state *old_dcstate)
{
	return old_dcstate->crc.source == DCIF_CRC_SRC_NONE &&
	       new_dcstate->crc.source != DCIF_CRC_SRC_NONE;
}

static inline bool disable_dcif_crc_needed(struct dcif_crtc_state *new_dcstate,
					   struct dcif_crtc_state *old_dcstate)
{
	return old_dcstate->crc.source != DCIF_CRC_SRC_NONE &&
	       new_dcstate->crc.source == DCIF_CRC_SRC_NONE;
}

static inline void dcif_copy_roi(struct drm_rect *from, struct drm_rect *to)
{
	to->x1 = from->x1;
	to->y1 = from->y1;
	to->x2 = from->x2;
	to->y2 = from->y2;
}

int dcif_crc_source_verify(struct drm_crtc *crtc, const char *source_name,
			   size_t *values_cnt);
int dcif_crc_source_set(struct drm_crtc *crtc, const char *source_name);
void dcif_crc_source_enable(struct dcif_dev *dcif, enum dcif_crc_source source,
			    struct drm_rect *roi, int ncrc);
void dcif_crc_source_disable(struct dcif_dev *dcif, int ncrc);

#endif /* __DCIF_CRC_H__ */
