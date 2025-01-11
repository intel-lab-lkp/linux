/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTEST_KVM_UTIL_ARCH_H
#define SELFTEST_KVM_UTIL_ARCH_H

#include "kvm_util_types.h"

struct kvm_vm_arch {};

enum kvm_riscv_stats {
	VCPU_STAT(ecall_exit_stat),
	VCPU_STAT(wfi_exit_stat),
	VCPU_STAT(wrs_exit_stat),
	VCPU_STAT(mmio_exit_user),
	VCPU_STAT(mmio_exit_kernel),
	VCPU_STAT(csr_exit_user),
	VCPU_STAT(csr_exit_kernel),
	VCPU_STAT(signal_exits),
	VCPU_STAT(exits),
};

#endif  // SELFTEST_KVM_UTIL_ARCH_H
