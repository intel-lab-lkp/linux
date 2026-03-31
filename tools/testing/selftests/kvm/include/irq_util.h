/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTEST_KVM_PROC_UTIL_H
#define SELFTEST_KVM_PROC_UTIL_H

#include <stdint.h>

int get_irq_number(const char *device_bdf, int msi);
uint64_t get_irq_count(int irq);
uint64_t get_irq_count_by_name(const char *name);

#endif /* SELFTEST_KVM_PROC_UTIL_H */
