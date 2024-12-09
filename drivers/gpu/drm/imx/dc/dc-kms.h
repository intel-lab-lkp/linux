/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2024 NXP
 */

#ifndef __DC_KMS_H__
#define __DC_KMS_H__

#include <linux/completion.h>

#include <drm/drm_crtc.h>
#include <drm/drm_plane.h>
#include <drm/drm_vblank.h>

#include "dc-de.h"
#include "dc-fu.h"
#include "dc-pe.h"

#define DC_CRTC_IRQS	5

struct dc_crtc_irq {
	struct dc_crtc *dc_crtc;
	unsigned int irq;
};

struct dc_crtc {
	struct drm_crtc base;
	struct dc_de *de;
	struct dc_pe *pe;
	struct dc_cf *cf_cont;
	struct dc_cf *cf_safe;
	struct dc_ed *ed_cont;
	struct dc_ed *ed_safe;
	struct dc_fg *fg;
	unsigned int irq_dec_framecomplete;
	unsigned int irq_dec_seqcomplete;
	unsigned int irq_dec_shdld;
	unsigned int irq_ed_cont_shdld;
	unsigned int irq_ed_safe_shdld;
	struct completion dec_seqcomplete_done;
	struct completion dec_shdld_done;
	struct completion ed_safe_shdld_done;
	struct completion ed_cont_shdld_done;
	struct drm_pending_vblank_event *event;
	struct dc_crtc_irq irqs[DC_CRTC_IRQS];
};

struct dc_plane {
	struct drm_plane base;
	struct dc_fu *fu;
	struct dc_cf *cf;
	struct dc_lb *lb;
	struct dc_ed *ed;
};

void dc_crtc_disable_at_unbind(struct drm_crtc *crtc);

#endif /* __DC_KMS_H__ */
