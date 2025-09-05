/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Header for sq and descriptor management
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#ifndef __SDXI_SQ_H
#define __SDXI_SQ_H

struct sdxi_cxt;
struct sdxi_dev;
struct sdxi_desc;

enum sdxi_cxt_id {
	SDXI_ADMIN_CXT_ID = 0,
	SDXI_DMA_CXT_ID = 1,
	SDXI_ANY_CXT_ID,
};

/* Context Control */
struct sdxi_cxt *sdxi_working_cxt_init(struct sdxi_dev *sdxi,
				       enum sdxi_cxt_id);
void sdxi_working_cxt_exit(struct sdxi_cxt *cxt);

int sdxi_submit_desc(struct sdxi_cxt *cxt, const struct sdxi_desc *desc);

#endif /* __SDXI_SQ_H */
