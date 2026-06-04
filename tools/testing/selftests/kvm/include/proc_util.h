/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTEST_KVM_PROC_UTIL_H
#define SELFTEST_KVM_PROC_UTIL_H

#include <stdint.h>

unsigned int vfio_msix_to_host_irq(const char *vfio_device_bdf, int msix);

FILE *open_proc_irq_smp_affinity_list(unsigned int irq);
void write_proc_irq_smp_affinity_list(FILE *fp, unsigned int irq, int irq_cpu);

void print_proc_irq_smp_affinity(unsigned int irq);
void print_proc_irq_effective_affinity(unsigned int irq);

#endif /* SELFTEST_KVM_PROC_UTIL_H */
