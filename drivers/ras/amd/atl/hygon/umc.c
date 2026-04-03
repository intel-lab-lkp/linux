// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * umc.c : Unified Memory Controller (UMC) topology helpers for Hygon
 *
 * Author: AichunShi <shiaichun@open-hieco.net>
 */

#include "../internal.h"

static u8 hygon_get_die_id(struct atl_err *err)
{
	/*
	 * For Hygon CPUs, amd_node_id can be non-contiguous, so get the Die
	 * ID from the logical Die ID.
	 */
	return topology_logical_die_id(err->cpu);
}

#define HYGON_IPID_SUB_CHANNEL_MASK	BIT(13)
static u8 hygon_get_ipid_sub_channel(struct atl_err *err)
{
	return FIELD_GET(HYGON_IPID_SUB_CHANNEL_MASK, err->ipid);
}

#define HYGON_UMC_CHANNEL_NUM	GENMASK(23, 20)
static u8 hygon_get_coh_st_inst_id(struct atl_err *err)
{
	u8 sub_channel = hygon_get_ipid_sub_channel(err);
	u8 coh_st_inst_id = FIELD_GET(HYGON_UMC_CHANNEL_NUM, err->ipid);

	if (df_cfg.rev == HYGON_DF2)
		coh_st_inst_id = (coh_st_inst_id << 1) + sub_channel;

	return coh_st_inst_id;
}

unsigned long hygon_convert_umc_mca_addr_to_sys_addr(struct atl_err *err)
{
	u8 socket_id = topology_physical_package_id(err->cpu);
	u8 coh_st_inst_id = hygon_get_coh_st_inst_id(err);
	u8 die_id = hygon_get_die_id(err);

	pr_debug("socket_id=0x%x die_id=0x%x coh_st_inst_id=0x%x addr=0x%016llx",
		 socket_id, die_id, coh_st_inst_id, err->addr);

	u8 sub_channel = hygon_get_ipid_sub_channel(err);

	return hygon_norm_to_sys_addr(socket_id, die_id, coh_st_inst_id,
				      sub_channel, err->addr);
}
