// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <dwarf-regs.h>
#include "../../../arch/sparc/include/uapi/asm/perf_regs.h"

int __get_dwarf_regnum_for_perf_regnum_sparc(int perf_regnum)
{
	if (perf_regnum < PERF_REG_SPARC_G0 || perf_regnum > PERF_REG_SPARC_O7)
		return -ENOENT;

	return perf_regnum;
}
