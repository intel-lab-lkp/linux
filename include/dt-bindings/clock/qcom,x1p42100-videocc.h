/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _DT_BINDINGS_CLK_QCOM_VIDEO_CC_X1P42100_H
#define _DT_BINDINGS_CLK_QCOM_VIDEO_CC_X1P42100_H

#include "qcom,sm8650-videocc.h"

/* X1P42100 introduces below new clocks and resets compared to SM8650 */

/* VIDEO_CC clocks */
#define VIDEO_CC_MVS0_BSE_CLK					17
#define VIDEO_CC_MVS0_BSE_CLK_SRC				18
#define VIDEO_CC_MVS0_BSE_DIV4_DIV_CLK_SRC			19

/* VIDEO_CC resets */
#define VIDEO_CC_MVS0_BSE_BCR					8

#endif
