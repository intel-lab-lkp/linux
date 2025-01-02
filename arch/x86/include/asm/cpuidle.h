/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPUIDLE_H
#define _ASM_X86_CPUIDLE_H

#include <asm/cpufeature.h>

static inline bool arch_cpuidle_mwait_needs_ipi(void)
{
	return boot_cpu_has_bug(X86_BUG_MONITOR);
}

#endif /* _ASM_X86_CPUIDLE_H */
