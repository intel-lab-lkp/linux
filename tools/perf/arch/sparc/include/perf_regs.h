/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARCH_PERF_REGS_H
#define ARCH_PERF_REGS_H

#include <stdlib.h>
#include <linux/types.h>
#include "../../../../arch/sparc/include/uapi/asm/perf_regs.h"

#define PERF_REGS_MASK	((1ULL << PERF_REG_SPARC_MAX) - 1)
#define PERF_REGS_MAX	PERF_REG_SPARC_MAX

#ifdef __arch64__
#define PERF_SAMPLE_REGS_ABI	PERF_SAMPLE_REGS_ABI_64
#else
#define PERF_SAMPLE_REGS_ABI	PERF_SAMPLE_REGS_ABI_32
#endif

#endif /* ARCH_PERF_REGS_H */
