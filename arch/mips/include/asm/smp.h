/*
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * Copyright (C) 2000 - 2001 by Kanoj Sarcar (kanoj@sgi.com)
 * Copyright (C) 2000 - 2001 by Silicon Graphics, Inc.
 * Copyright (C) 2000, 2001, 2002 Ralf Baechle
 * Copyright (C) 2000, 2001 Broadcom Corporation
 */
#ifndef __ASM_SMP_H
#define __ASM_SMP_H

#include <linux/compiler.h>
#include <linux/linkage.h>
#include <linux/threads.h>
#include <linux/cpumask.h>

extern int smp_num_siblings;
extern cpumask_t cpu_sibling_map[];
extern cpumask_t cpu_core_map[];
extern cpumask_t cpu_foreign_map[];

static inline int raw_smp_processor_id(void)
{
#if defined(__VDSO__)
	extern int vdso_smp_processor_id(void)
		__compiletime_error("VDSO should not call smp_processor_id()");
	return vdso_smp_processor_id();
#else
	return current_thread_info()->cpu;
#endif
}
#define raw_smp_processor_id raw_smp_processor_id

/* Map from cpu id to sequential logical cpu number.  This will only
   not be idempotent when cpus failed to come on-line.	*/
extern int __cpu_number_map[CONFIG_MIPS_NR_CPU_NR_MAP];
#define cpu_number_map(cpu)  __cpu_number_map[cpu]

/* The reverse map from sequential logical cpu number to cpu id.  */
extern int __cpu_logical_map[NR_CPUS];
#define cpu_logical_map(cpu)  __cpu_logical_map[cpu]

#define NO_PROC_ID	(-1)

/* Mask of CPUs which are currently definitely operating coherently */
extern cpumask_t cpu_coherent_mask;

extern unsigned int smp_max_threads __initdata;

extern asmlinkage void smp_bootstrap(void);

extern void calculate_cpu_foreign_map(void);

asmlinkage void start_secondary(void);

enum ipi_message_type {
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
#ifdef CONFIG_CAVIUM_OCTEON_SOC
	IPI_ICACHE_FLUSH,
#endif
#ifdef CONFIG_MACH_LOONGSON64
	IPI_ASK_C0COUNT,
#endif
	IPI_MAX
};

#include <asm/smp-ops.h>

/*
 * this function sends a 'reschedule' IPI to another CPU.
 * it goes straight through and wastes no time serializing
 * anything. Worst case is that we lose a reschedule ...
 */
static inline void arch_smp_send_reschedule(int cpu)
{
	extern const struct plat_smp_ops *mp_ops;	/* private */

	mp_ops->send_ipi_single(cpu, IPI_RESCHEDULE);
}

#ifdef CONFIG_HOTPLUG_CPU
static inline int __cpu_disable(void)
{
	extern const struct plat_smp_ops *mp_ops;	/* private */

	return mp_ops->cpu_disable();
}

static inline void __cpu_die(unsigned int cpu)
{
	extern const struct plat_smp_ops *mp_ops;	/* private */

	mp_ops->cpu_die(cpu);
}

extern void __noreturn play_dead(void);
#endif

#ifdef CONFIG_KEXEC_CORE
static inline void kexec_nonboot_cpu(void)
{
	extern const struct plat_smp_ops *mp_ops;	/* private */

	return mp_ops->kexec_nonboot_cpu();
}

static inline void *kexec_nonboot_cpu_func(void)
{
	extern const struct plat_smp_ops *mp_ops;	/* private */

	return mp_ops->kexec_nonboot_cpu;
}
#endif

static inline void arch_send_call_function_single_ipi(int cpu)
{
	extern const struct plat_smp_ops *mp_ops;	/* private */

	mp_ops->send_ipi_single(cpu, IPI_CALL_FUNC);
}

static inline void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	extern const struct plat_smp_ops *mp_ops;	/* private */

	mp_ops->send_ipi_mask(mask, IPI_CALL_FUNC);
}

#endif /* __ASM_SMP_H */
