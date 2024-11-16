/* SPDX-License-Identifier: MIT */

#ifndef __DAL_COMMAND_TABLE_HELPER_DCE_COMMON_H__
#define __DAL_COMMAND_TABLE_HELPER_DCE_COMMON_H__


uint8_t phy_id_to_atom(enum transmitter t);

uint8_t clock_source_id_to_atom_phy_clk_src_id(
		enum clock_source_id id);

bool engine_bp_to_atom(enum engine_id id, uint32_t *atom_engine_id);

#endif
