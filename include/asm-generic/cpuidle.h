/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_CPUIDLE_H
#define __ASM_CPUIDLE_H

static inline bool arch_cpuidle_mwait_needs_ipi(void)
{
	return true;
}

#endif /* __ASM_CPUIDLE_H */
