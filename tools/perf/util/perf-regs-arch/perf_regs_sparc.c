// SPDX-License-Identifier: GPL-2.0

#include "../perf_regs.h"
#include "../../arch/sparc/include/perf_regs.h"

uint64_t __perf_reg_mask_sparc(bool intr __maybe_unused)
{
	return PERF_REGS_MASK;
}

const char *__perf_reg_name_sparc(int id)
{
	switch (id) {
	case PERF_REG_SPARC_G0:
		return "g0";
	case PERF_REG_SPARC_G1:
		return "g1";
	case PERF_REG_SPARC_G2:
		return "g2";
	case PERF_REG_SPARC_G3:
		return "g3";
	case PERF_REG_SPARC_G4:
		return "g4";
	case PERF_REG_SPARC_G5:
		return "g5";
	case PERF_REG_SPARC_G6:
		return "g6";
	case PERF_REG_SPARC_G7:
		return "g7";
	case PERF_REG_SPARC_O0:
		return "o0";
	case PERF_REG_SPARC_O1:
		return "o1";
	case PERF_REG_SPARC_O2:
		return "o2";
	case PERF_REG_SPARC_O3:
		return "o3";
	case PERF_REG_SPARC_O4:
		return "o4";
	case PERF_REG_SPARC_O5:
		return "o5";
	case PERF_REG_SPARC_O6:
		return "sp";
	case PERF_REG_SPARC_O7:
		return "o7";
	case PERF_REG_SPARC_TSTATE:
		return "tstate";
	case PERF_REG_SPARC_PC:
		return "pc";
	case PERF_REG_SPARC_NPC:
		return "npc";
	case PERF_REG_SPARC_Y:
		return "y";
	default:
		return NULL;
	}

	return NULL;
}

uint64_t __perf_reg_ip_sparc(void)
{
	return PERF_REG_SPARC_PC;
}

uint64_t __perf_reg_sp_sparc(void)
{
	return PERF_REG_SPARC_O6;
}
