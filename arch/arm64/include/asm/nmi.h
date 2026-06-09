/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_NMI_H
#define __ASM_NMI_H

#include <linux/cpumask.h>

struct pt_regs;

/*
 * Cross-CPU NMI provider hooks, consulted by the arm64 arch code before
 * its regular-IRQ / pseudo-NMI IPI paths. The SDEI provider in
 * drivers/firmware/arm_sdei_nmi.c implements them when active; a future
 * FEAT_NMI provider could slot in here too. The stubs let callers stay
 * unconditional when ARM_SDEI_NMI is off.
 *
 * arm64_nmi_cpu_stop() is the reverse direction: the arch entry point
 * (arch/arm64/kernel/smp.c) that the provider's NMI handler routes a
 * stop request into.
 */
#ifdef CONFIG_ARM_SDEI_NMI
bool sdei_nmi_trigger_cpumask_backtrace(const cpumask_t *mask, int exclude_cpu);
bool sdei_nmi_stop_cpus(const cpumask_t *mask);

void __noreturn arm64_nmi_cpu_stop(struct pt_regs *regs);
#else
static inline bool sdei_nmi_trigger_cpumask_backtrace(const cpumask_t *mask,
						      int exclude_cpu)
{
	return false;
}

static inline bool sdei_nmi_stop_cpus(const cpumask_t *mask)
{
	return false;
}
#endif

#endif /* __ASM_NMI_H */
