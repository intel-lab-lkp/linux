/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/cpumask.h>
#include <linux/interrupt.h>

#ifndef __ASM_IPI_H
#define __ASM_IPI_H

#ifdef CONFIG_SMP
extern const char *ipi_names[];
extern irq_handler_t ipi_handlers[];

#ifdef CONFIG_GENERIC_IRQ_IPI
extern void mips_smp_send_ipi_single(int cpu,
				     enum ipi_message_type op);
extern void mips_smp_send_ipi_mask(const struct cpumask *mask,
				   enum ipi_message_type op);

/*
 * This function will set up the necessary IPIs for Linux to communicate
 * with the CPUs in mask.
 * Return 0 on success.
 */
int mips_smp_ipi_allocate(const struct cpumask *mask);

/*
 * This function will free up IPIs allocated with mips_smp_ipi_allocate to the
 * CPUs in mask, which must be a subset of the IPIs that have been configured.
 * Return 0 on success.
 */
int mips_smp_ipi_free(const struct cpumask *mask);
#endif /* CONFIG_GENERIC_IRQ_IPI */
#endif /* CONFIG_SMP */
#endif
