/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARCH_PERF_REGS_H
#define ARCH_PERF_REGS_H

#include <stdlib.h>
#include <linux/types.h>
#include <linux/bits.h>
#include "../../../../arch/alpha/include/uapi/asm/perf_regs.h"

#define PERF_REGS_MAX PERF_REG_ALPHA_MAX

/* The kernel rejects r9-r15. */
#define PERF_REGS_MASK (GENMASK_ULL(PERF_REG_ALPHA_MAX - 1, 0) & \
			~GENMASK_ULL(PERF_REG_ALPHA_R15, PERF_REG_ALPHA_R9))

#endif /* ARCH_PERF_REGS_H */
