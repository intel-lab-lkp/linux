/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTEST_KVM_UCALL_H
#define SELFTEST_KVM_UCALL_H

#include "kvm_util_base.h"

#define UCALL_EXIT_REASON       KVM_EXIT_MMIO

/*
 * Default base address for application loading is 0x120000000,
 * DEFAULT_GUEST_TEST_MEM should be larger than app loading address,
 * so that PER_VCPU_MEM_SIZE can be large enough, and kvm selftests
 * app size is smaller than 256M in generic
 */
#define DEFAULT_GUEST_TEST_MEM	0x130000000

/*
 * ucall_exit_mmio_addr holds per-VM values (global data is duplicated by each
 * VM), it must not be accessed from host code.
 */
extern vm_vaddr_t *ucall_exit_mmio_addr;

static inline void ucall_arch_do_ucall(vm_vaddr_t uc)
{
	WRITE_ONCE(*ucall_exit_mmio_addr, uc);
}

#endif
