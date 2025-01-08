/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_BUGS_H
#define _ASM_X86_BUGS_H

#include <asm/processor.h>

#if defined(CONFIG_CPU_SUP_INTEL) && defined(CONFIG_X86_32)
int ppro_with_ram_bug(void);
#else
static inline int ppro_with_ram_bug(void) { return 0; }
#endif

extern void cpu_bugs_smt_update(void);

enum cpu_attack_vectors {
	CPU_MITIGATE_USER_KERNEL,
	CPU_MITIGATE_USER_USER,
	CPU_MITIGATE_GUEST_HOST,
	CPU_MITIGATE_GUEST_GUEST,
	CPU_MITIGATE_CROSS_THREAD,
	NR_CPU_ATTACK_VECTORS,
};

bool cpu_mitigate_attack_vector(enum cpu_attack_vectors v);

#endif /* _ASM_X86_BUGS_H */
