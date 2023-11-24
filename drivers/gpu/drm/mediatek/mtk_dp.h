/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2023 MediaTek Inc.
 */

#ifndef _MTK_DP_H_
#define _MTK_DP_H_

#include "mtk_dp_hdcp.h"

u32 mtk_dp_reg_read(struct regmap *regs, u32 offset);

int mtk_dp_reg_update_bits(struct regmap *regs, u32 offset, u32 val, u32 mask);

void mtk_dp_re_authentication(struct mtk_hdcp_info *hdcp_info);

#endif /* _MTK_DP_H_ */
