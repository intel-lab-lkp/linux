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

#endif /* __INTEL_CASF_REGS__ */

