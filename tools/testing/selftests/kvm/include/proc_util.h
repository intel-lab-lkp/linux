/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTEST_KVM_PROC_UTIL_H
#define SELFTEST_KVM_PROC_UTIL_H

#include <stdint.h>

int get_proc_vfio_irq_number(const char *vfio_device_bdf, int msi);

#endif /* SELFTEST_KVM_PROC_UTIL_H */
