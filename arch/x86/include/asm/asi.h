/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_ASI_H
#define _ASM_X86_ASI_H

#include <asm/cpufeature.h>

void asi_check_boottime_disable(void);

/* Helper for generic code. Arch code just uses cpu_feature_enabled(). */
static inline bool asi_enabled_static(void)
{
	return cpu_feature_enabled(X86_FEATURE_ASI);
}

#endif /* _ASM_X86_ASI_H */
