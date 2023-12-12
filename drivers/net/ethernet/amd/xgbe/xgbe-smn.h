/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD 10Gb Ethernet driver
 *
 * Copyright (c) 2023, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Raju Rangoju <Raju.Rangoju@amd.com>
 */

#ifdef CONFIG_AMD_NB

#include <asm/amd_nb.h>

#else

static inline int amd_smn_write(u16 node, u32 address, u32 value)
{
	return -ENODEV;
}

static inline int amd_smn_read(u16 node, u32 address, u32 *value)
{
	return -ENODEV;
}

#endif
