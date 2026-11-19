// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include "mali-vpu-dec.h"
#include "mali-vpu-session.h"
#include "mali-vpu-variant.h"

void mali_vpu_session_init_dec(struct mali_vpu_session *sess)
{
	const struct mali_vpu_format_caps *variant_fmts = &sess->vpu_dev->variant->fmt_caps;

	sess->port_src.supported_fmts = variant_fmts->dec_src;
	sess->port_dst.supported_fmts = variant_fmts->dec_dst;
}
