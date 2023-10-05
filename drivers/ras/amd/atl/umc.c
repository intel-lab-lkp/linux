// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * umc.c : Unified Memory Controller (UMC) topology helpers
 *
 * Copyright (c) 2023, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Yazen Ghannam <Yazen.Ghannam@amd.com>
 */

#include "internal.h"

static u8 get_socket_id(struct mce *m)
{
	return m->socketid;
}

static u8 get_die_id(struct mce *m)
{
	/*
	 * For CPUs, this is the AMD Node ID modulo the number
	 * of AMD Nodes per socket.
	 */
	return topology_die_id(m->extcpu) % amd_get_nodes_per_socket();
}

static u64 get_norm_addr(struct mce *m)
{
	return m->addr;
}

#define UMC_CHANNEL_NUM	GENMASK(31, 20)
static u8 get_cs_inst_id(struct mce *m)
{
	return FIELD_GET(UMC_CHANNEL_NUM, m->ipid);
}

int umc_mca_addr_to_sys_addr(struct mce *m, u64 *sys_addr)
{
	u8 cs_inst_id = get_cs_inst_id(m);
	u8 socket_id = get_socket_id(m);
	u64 addr = get_norm_addr(m);
	u8 die_id = get_die_id(m);

	if (norm_to_sys_addr(socket_id, die_id, cs_inst_id, &addr))
		return -EINVAL;

	*sys_addr = addr;
	return 0;
}
EXPORT_SYMBOL_GPL(umc_mca_addr_to_sys_addr);
