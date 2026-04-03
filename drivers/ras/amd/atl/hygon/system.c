// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * system.c : Functions to read and save system-wide data for Hygon
 *
 * Author: AichunShi <shiaichun@open-hieco.net>
 */

#include "../internal.h"

static void hygon_df_get_masks_shifts(u32 mask0)
{
	switch (df_cfg.rev) {
	case HYGON_DF1:
		df_cfg.socket_id_shift = FIELD_GET(HYGON_DF1_SOCKET_ID_SHIFT, mask0);
		df_cfg.socket_id_mask = FIELD_GET(HYGON_DF1_SOCKET_ID_MASK, mask0);
		df_cfg.die_id_shift = FIELD_GET(HYGON_DF1_DIE_ID_SHIFT, mask0);
		df_cfg.die_id_mask = FIELD_GET(HYGON_DF1_DIE_ID_MASK, mask0);
		break;
	case HYGON_DF2:
		df_cfg.socket_id_shift = FIELD_GET(HYGON_DF1_SOCKET_ID_SHIFT, mask0);
		df_cfg.socket_id_mask = FIELD_GET(HYGON_DF2_SOCKET_ID_MASK, mask0);
		df_cfg.die_id_shift = FIELD_GET(HYGON_DF1_DIE_ID_SHIFT, mask0);
		df_cfg.die_id_mask = FIELD_GET(HYGON_DF2_DIE_ID_MASK, mask0);
		break;
	case HYGON_DF3:
		df_cfg.socket_id_shift = FIELD_GET(HYGON_DF1_SOCKET_ID_SHIFT, mask0);
		df_cfg.socket_id_mask = FIELD_GET(HYGON_DF3_SOCKET_ID_MASK, mask0);
		df_cfg.die_id_shift = FIELD_GET(HYGON_DF3_DIE_ID_SHIFT, mask0);
		df_cfg.die_id_mask = FIELD_GET(HYGON_DF3_DIE_ID_MASK, mask0);
		break;
	default:
		atl_debug_on_bad_df_rev();
		return;
	}
}

static int hygon_determine_df_rev(void)
{
	u32 fabric_id_mask0;

	if (boot_cpu_data.x86_model == 0x4 || boot_cpu_data.x86_model == 0x5)
		df_cfg.rev = HYGON_DF1;
	else if (boot_cpu_data.x86_model == 0x6 || boot_cpu_data.x86_model == 0x8)
		df_cfg.rev = HYGON_DF2;
	else if (boot_cpu_data.x86_model == 0x7)
		df_cfg.rev = HYGON_DF3;

	/* Read D18F1x208 (SystemFabricIdMask). */
	if (df_indirect_read_broadcast(0, 1, 0x208, &fabric_id_mask0))
		return -EINVAL;

	hygon_df_get_masks_shifts(fabric_id_mask0);

	return 0;
}

static void hygon_get_num_maps(void)
{
	switch (df_cfg.rev) {
	case HYGON_DF1:
	case HYGON_DF2:
	case HYGON_DF3:
		df_cfg.num_coh_st_maps	= 2;
		break;
	default:
		atl_debug_on_bad_df_rev();
	}
}

int hygon_get_df_system_info(void)
{
	int ret;

	ret = hygon_determine_df_rev();
	if (ret) {
		pr_warn("Failed to determine DF Revision");
		df_cfg.rev = UNKNOWN;
		return ret;
	}

	hygon_get_num_maps();

	if (get_dram_hole_base())
		pr_warn("Failed to read DRAM hole base");

	dump_df_cfg();

	return 0;
}
