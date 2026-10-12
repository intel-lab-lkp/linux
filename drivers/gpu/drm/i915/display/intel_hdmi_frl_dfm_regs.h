/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __INTEL_HDMI_FRL_DFM_REGS_H__
#define __INTEL_HDMI_FRL_DFM_REGS_H__

#include "intel_display_reg_defs.h"

#define _TRANS_HDMI_FRL_DFMRDCTL_A			0x600bc
#define TRANS_HDMI_FRL_DFMRDCTL(dev_priv, trans)	_MMIO_TRANS2(dev_priv, \
								     trans, \
								     _TRANS_HDMI_FRL_DFMRDCTL_A)
#define   TB_DIFF_MAX_OFFSET_MASK			REG_GENMASK(8, 0)
#define   TB_DIFF_MAX_OFFSET(val)			REG_FIELD_PREP(TB_DIFF_MAX_OFFSET_MASK, val)

#define _TRANS_HDMI_FRL_DFMWRCTL_A			0x600c0
#define TRANS_HDMI_FRL_DFMWRCTL(dev_priv, trans)	_MMIO_TRANS2(dev_priv, \
								     trans, \
								     _TRANS_HDMI_FRL_DFMWRCTL_A)
#define  TB_ACTUAL_OFFSET_MASK				REG_GENMASK(8, 0)
#define  TB_ACTUAL_OFFSET(val)				REG_FIELD_PREP(TB_ACTUAL_OFFSET_MASK, val)

#define _TRANS_HDMI_FRL_DFMTHRSH_A			0x600c4
#define TRANS_HDMI_FRL_DFMTHRSH(dev_priv, trans)	_MMIO_TRANS2(dev_priv, \
								     trans, \
								     _TRANS_HDMI_FRL_DFMTHRSH_A)
#define  RUN_LEN_THRESHOLD_MASK				REG_GENMASK(31, 24)
#define  RUN_LEN_THRESHOLD(val)				REG_FIELD_PREP(RUN_LEN_THRESHOLD_MASK, val)
#define  TB_MAX_THRESHOLD_MASK				REG_GENMASK(21, 12)
#define  TB_MAX_THRESHOLD(val)				REG_FIELD_PREP(TB_MAX_THRESHOLD_MASK, val)
#define  TB_MIN_THESHOLD_MASK				REG_GENMASK(9, 0)
#define  TB_MIN_THESHOLD(val)				REG_FIELD_PREP(TB_MIN_THESHOLD_MASK, val)

#endif /* __INTEL_HDMI_FRL_DFM_REGS_H__ */
