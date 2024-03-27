/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 1999 - 2024 Intel Corporation. */

#ifndef _IXGBE_X550_H_
#define _IXGBE_X550_H_

#include "ixgbe_type.h"

extern const u32 ixgbe_mvals_x550em_a[IXGBE_MVALS_IDX_LIMIT];

s32 ixgbe_set_fw_drv_ver_x550(struct ixgbe_hw *hw, u8 maj, u8 min,
			      u8 build, u8 sub, u16 len,
			      const char *driver_ver);

#endif /* _IXGBE_X550_H_ */
