/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_CASF_REGS_H__
#define __INTEL_CASF_REGS_H__

#include "intel_display_reg_defs.h"

/* Scaler Coefficient structure */
#define SIGN				REG_BIT(15)
#define EXPONENT_MASK			REG_GENMASK(13, 12)
#define EXPONENT(x)			REG_FIELD_PREP(EXPONENT_MASK, (x))
#define MANTISSA_MASK			REG_GENMASK(11, 3)
#define MANTISSA(x)			REG_FIELD_PREP(MANTISSA_MASK, (x))

#define _SHARPNESS_CTL_A                0x682B0
#define SHARPNESS_CTL(display, trans)   _MMIO_PIPE2(display, trans, _SHARPNESS_CTL_A)
#define   FILTER_EN                     REG_BIT(31)
#define   FILTER_STRENGTH_MASK          REG_GENMASK(15, 8)
#define   FILTER_STRENGTH(x)            REG_FIELD_PREP(FILTER_STRENGTH_MASK, (x))
#define   FILTER_SIZE_MASK              REG_GENMASK(1, 0)
#define   FILTER_SIZE(x)                REG_FIELD_PREP(FILTER_SIZE_MASK, (x))

#define _SHRPLUT_DATA_A                 0x682B8
#define SHRPLUT_DATA(display, trans)    _MMIO_PIPE2(display, trans, _SHRPLUT_DATA_A)

#define _SHRPLUT_INDEX_A                0x682B4
#define SHRPLUT_INDEX(display, trans)   _MMIO_PIPE2(display, trans, _SHRPLUT_INDEX_A)
#define   INDEX_AUTO_INCR               REG_BIT(10)
#define   INDEX_VALUE_MASK              REG_GENMASK(4, 0)
#define   INDEX_VALUE(x)                REG_FIELD_PREP(INDEX_VALUE_MASK, (x))

#endif /* __INTEL_CASF_REGS__ */

