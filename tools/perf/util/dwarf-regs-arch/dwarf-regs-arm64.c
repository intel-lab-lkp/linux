// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <ctype.h>
#include <dwarf-regs.h>
#include "../../../arch/arm64/include/uapi/asm/perf_regs.h"

int __get_dwarf_regnum_for_perf_regnum_arm64(int perf_regnum)
{
	if (perf_regnum < 0 || perf_regnum >= PERF_REG_ARM64_MAX)
		return -ENOENT;

	return perf_regnum;
}

int __get_dwarf_regnum_arm64(const char *name)
{
	int reg;

	if (!strcmp(name, "sp") || !strcmp(name, "wzr") || !strcmp(name, "xzr"))
		return 31;

	if (*name != 'x' && *name != 'w')
		return -ENOENT;

	name++;
	if (!isdigit(*name))
		return -ENOENT;

	reg = strtol(name, NULL, 10);

	return reg >= 0 && reg <= 30 ? reg : -ENOENT;
}
