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

#define MCA_IPID_INST_ID_HI	GENMASK_ULL(47, 44)
static u8 get_die_id(struct mce *m)
{
	/* The "AMD Node ID" is provided in MCA_IPID[InstanceIdHi] */
	if (df_cfg.rev == DF4p5 && df_cfg.flags.heterogeneous) {
		u8 node_id = FIELD_GET(MCA_IPID_INST_ID_HI, m->ipid);

		return node_id / 4;
	}

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

/*
 * Use CPU's AMD Node ID for all cases.
 *
 * This is needed to read DF registers which can only be
 * done on CPU-attached DFs even in heterogeneous cases.
 *
 * Future systems may report MCA errors across AMD Nodes.
 * For example, errors from CPU socket 1 are reported to a
 * CPU on socket 0. When this happens, the assumption below
 * will break. But the AMD Node ID will be reported in
 * MCA_IPID[InstanceIdHi] at that time.
 */
static u16 get_df_acc_id(struct mce *m)
{
	return topology_die_id(m->extcpu);
}

int umc_mca_addr_to_sys_addr(struct mce *m, u64 *sys_addr)
{
	u8 cs_inst_id = get_cs_inst_id(m);
	u8 socket_id = get_socket_id(m);
	u64 addr = get_norm_addr(m);
	u8 die_id = get_die_id(m);
	u16 df_acc_id = get_df_acc_id(m);

	if (norm_to_sys_addr(df_acc_id, socket_id, die_id, cs_inst_id, &addr))
		return -EINVAL;

	*sys_addr = addr;
	return 0;
}
EXPORT_SYMBOL_GPL(umc_mca_addr_to_sys_addr);
