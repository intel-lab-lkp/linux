/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AMD Address Translation Library
 *
 * Copyright (c) 2023, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Yazen Ghannam <Yazen.Ghannam@amd.com>
 */

#ifndef _AMD_ATL_H
#define _AMD_ATL_H

#include <uapi/asm/mce.h>

int umc_mca_addr_to_sys_addr(struct mce *m, u64 *sys_addr);

static inline int amd_umc_mca_addr_to_sys_addr(struct mce *m, u64 *sys_addr)
{
	if (!IS_REACHABLE(CONFIG_AMD_ATL)) {
		pr_warn("AMD Address Translation Library not available");
		return -EINVAL;
	}

	return umc_mca_addr_to_sys_addr(m, sys_addr);
}

#endif /* _AMD_ATL_H */
