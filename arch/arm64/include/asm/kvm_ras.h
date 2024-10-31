/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018 - Arm Ltd */

#ifndef __ARM64_KVM_RAS_H__
#define __ARM64_KVM_RAS_H__

#include <linux/acpi.h>
#include <linux/errno.h>
#include <linux/types.h>

#include <asm/acpi.h>

/*
 * Handle synchronous external abort (SEA) in the following order:
 * 1. Delegate to APEI/GHES to see if SEA can be claimed by them. If so, we
 *    are all done.
 * 2. If userspace opts in KVM_CAP_ARM_SIGBUS_ON_SEA, and if the SEA is NOT
 *    about translation table, send SIGBUS
 *    - si_code is BUS_OBJERR.
 *    - si_addr will be 0 when accurate HVA is unavailable.
 * 3. Otherwise, directly inject an async SError to guest.
 *
 * Note this applies to both ESR_ELx_EC_IABT_* and ESR_ELx_EC_DABT_*.
 */
void kvm_handle_guest_sea(struct kvm_vcpu *vcpu);

#endif /* __ARM64_KVM_RAS_H__ */
