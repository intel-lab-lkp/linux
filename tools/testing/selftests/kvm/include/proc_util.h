/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTEST_KVM_PROC_UTIL_H
#define SELFTEST_KVM_PROC_UTIL_H

#include <stdint.h>

int get_proc_vfio_irq_number(const char *vfio_device_bdf, int msi);

/*
 * open_proc_irq_affinity - Open the smp_affinity_list file for a given IRQ
 * @irq: The IRQ number
 *
 * Opens /proc/irq/<irq>/smp_affinity_list for writing and returns the FILE
 * pointer.
 */
FILE *open_proc_irq_affinity(int irq);

/*
 * write_proc_irq_affinity - Write a CPU number to the smp_affinity_list file
 * @fp: The FILE pointer for the smp_affinity_list file
 * @irq: The IRQ number (for error reporting)
 * @irq_cpu: The CPU number to write
 *
 * Writes the given CPU number to the provided FILE pointer.
 */
void write_proc_irq_affinity(FILE *fp, int irq, int irq_cpu);

#endif /* SELFTEST_KVM_PROC_UTIL_H */
