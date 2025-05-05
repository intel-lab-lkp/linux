/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018 - Arm Ltd */

#ifndef __ARM64_KVM_RAS_H__
#define __ARM64_KVM_RAS_H__

#include <linux/kvm_host.h>

/*
 * Handle stage2 synchronous external abort (SEA) in the following order:
 * 1. Delegate to APEI/GHES and if they can claim SEA, resume guest.
 * 2. If userspace opt-ed in KVM_CAP_ARM_SEA_TO_USER, exit to userspace
 *    with details about the SEA.
 * 3. Otherwise, inject async SError into the VCPU and resume guest.
 */
int kvm_handle_guest_sea(struct kvm_vcpu *vcpu);

#endif /* __ARM64_KVM_RAS_H__ */
