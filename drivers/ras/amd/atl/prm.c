// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * prm.c : Plumbing code for ACPI Platform Runtime Mechanism (PRM)
 *
 * Information on AMD PRM modules and handlers including the GUIDs and buffer
 * structures used here are defined in the AMD ACPI Porting Guide in the
 * chapter "Platform Runtime Mechanism Table (PRMT)"
 *
 * Copyright (c) 2024, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: John Allen <john.allen@amd.com>
 */

#include "internal.h"

#include <linux/prmt.h>

/* See "PRM Parameter Buffer" in the AMD ACPI Porting Guide. */
struct param_buf {
	u64 norm_addr;
	u8 socket;
	u64 bank_id;
	void *out_buf;
} __packed;

int prm_umc_norm_to_addr(guid_t guid, u8 socket_id, u64 bank_id,
			 unsigned long addr, void *out_buf)
{
	struct param_buf p_buf;
	int ret;

	p_buf.norm_addr = addr;
	p_buf.socket    = socket_id;
	p_buf.bank_id   = bank_id;
	p_buf.out_buf   = out_buf;

	ret = acpi_call_prm_handler(guid, &p_buf);
	if (!ret)
		return 0;

	if (ret == -ENODEV || ret == -EOPNOTSUPP)
		pr_debug("PRM module/handler not available: %d\n", ret);
	else
		pr_notice_once("PRM address translation failed: %d\n", ret);

	return ret;
}

unsigned long prm_umc_norm_to_sys_addr(u8 socket_id, u64 bank_id, unsigned long addr)
{
	unsigned long sys_addr;
	int ret;

	ret = prm_umc_norm_to_addr(norm_to_sys_guid, socket_id, bank_id, addr, &sys_addr);
	if (ret)
		return ret;

	return sys_addr;
}
