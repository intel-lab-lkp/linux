// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <dwarf-regs.h>
#include "../../../arch/alpha/include/uapi/asm/perf_regs.h"

int __get_dwarf_regnum_for_perf_regnum_alpha(int perf_regnum)
{
	if (perf_regnum >= PERF_REG_ALPHA_R0 && perf_regnum <= PERF_REG_ALPHA_SP)
		return perf_regnum;
	if (perf_regnum == PERF_REG_ALPHA_PC)
		return 64;

	return -ENOENT;
}
