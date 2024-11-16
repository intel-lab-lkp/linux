// SPDX-License-Identifier: MIT

#include "dm_services.h"
#include "atom.h"

#include "command_table_helper_dce_common.h"

uint8_t phy_id_to_atom(enum transmitter t)
{
	uint8_t atom_phy_id;

	switch (t) {
	case TRANSMITTER_UNIPHY_A:
		atom_phy_id = ATOM_PHY_ID_UNIPHYA;
		break;
	case TRANSMITTER_UNIPHY_B:
		atom_phy_id = ATOM_PHY_ID_UNIPHYB;
		break;
	case TRANSMITTER_UNIPHY_C:
		atom_phy_id = ATOM_PHY_ID_UNIPHYC;
		break;
	case TRANSMITTER_UNIPHY_D:
		atom_phy_id = ATOM_PHY_ID_UNIPHYD;
		break;
	case TRANSMITTER_UNIPHY_E:
		atom_phy_id = ATOM_PHY_ID_UNIPHYE;
		break;
	case TRANSMITTER_UNIPHY_F:
		atom_phy_id = ATOM_PHY_ID_UNIPHYF;
		break;
	case TRANSMITTER_UNIPHY_G:
		atom_phy_id = ATOM_PHY_ID_UNIPHYG;
		break;
	default:
		atom_phy_id = ATOM_PHY_ID_UNIPHYA;
		break;
	}
	return atom_phy_id;
}

uint8_t clock_source_id_to_atom_phy_clk_src_id(
		enum clock_source_id id)
{
	uint8_t atom_phy_clk_src_id = 0;

	switch (id) {
	case CLOCK_SOURCE_ID_PLL0:
		atom_phy_clk_src_id = ATOM_TRANSMITTER_CONFIG_V5_P0PLL;
		break;
	case CLOCK_SOURCE_ID_PLL1:
		atom_phy_clk_src_id = ATOM_TRANSMITTER_CONFIG_V5_P1PLL;
		break;
	case CLOCK_SOURCE_ID_PLL2:
		atom_phy_clk_src_id = ATOM_TRANSMITTER_CONFIG_V5_P2PLL;
		break;
	case CLOCK_SOURCE_ID_EXTERNAL:
		atom_phy_clk_src_id = ATOM_TRANSMITTER_CONFIG_V5_REFCLK_SRC_EXT;
		break;
	default:
		atom_phy_clk_src_id = ATOM_TRANSMITTER_CONFIG_V5_P1PLL;
		break;
	}

	return atom_phy_clk_src_id >> 2;
}

bool engine_bp_to_atom(enum engine_id id, uint32_t *atom_engine_id)
{
	bool result = false;

	if (atom_engine_id != NULL)
		switch (id) {
		case ENGINE_ID_DIGA:
			*atom_engine_id = ASIC_INT_DIG1_ENCODER_ID;
			result = true;
			break;
		case ENGINE_ID_DIGB:
			*atom_engine_id = ASIC_INT_DIG2_ENCODER_ID;
			result = true;
			break;
		case ENGINE_ID_DIGC:
			*atom_engine_id = ASIC_INT_DIG3_ENCODER_ID;
			result = true;
			break;
		case ENGINE_ID_DIGD:
			*atom_engine_id = ASIC_INT_DIG4_ENCODER_ID;
			result = true;
			break;
		case ENGINE_ID_DIGE:
			*atom_engine_id = ASIC_INT_DIG5_ENCODER_ID;
			result = true;
			break;
		case ENGINE_ID_DIGF:
			*atom_engine_id = ASIC_INT_DIG6_ENCODER_ID;
			result = true;
			break;
		case ENGINE_ID_DIGG:
			*atom_engine_id = ASIC_INT_DIG7_ENCODER_ID;
			result = true;
			break;
		case ENGINE_ID_DACA:
			*atom_engine_id = ASIC_INT_DAC1_ENCODER_ID;
			result = true;
			break;
		default:
			break;
		}

	return result;
}
