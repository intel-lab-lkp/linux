// SPDX-License-Identifier: GPL-2.0

#include "../perf_regs.h"
#include "../../arch/alpha/include/perf_regs.h"

static const char * const alpha_reg_names[PERF_REG_ALPHA_MAX] = {
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
	"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
	"r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
	"r24", "r25", "r26", "r27", "r28", "gp", "sp", "pc",
};

uint64_t __perf_reg_mask_alpha(bool intr __maybe_unused)
{
	return PERF_REGS_MASK;
}

const char *__perf_reg_name_alpha(int id)
{
	if (id < 0 || id >= PERF_REG_ALPHA_MAX)
		return NULL;

	return alpha_reg_names[id];
}

uint64_t __perf_reg_ip_alpha(void)
{
	return PERF_REG_ALPHA_PC;
}

uint64_t __perf_reg_sp_alpha(void)
{
	return PERF_REG_ALPHA_SP;
}
